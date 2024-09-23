#include <dirent.h>
#include <sys/resource.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "unqlite.h"

//  Define macro to control the output of debug information, enabled by default
#ifndef ENABLE_DEBUG_OUTPUT
#define ENABLE_DEBUG_OUTPUT 0
#endif

#if ENABLE_DEBUG_OUTPUT
#define DEBUG_PRINT(fmt, ...) fprintf(stdout, fmt, __VA_ARGS__)
#else
#define DEBUG_PRINT(fmt, ...)
#endif

#if defined(__aarch64__)
#define PRIORITY_FIELD_INDEX 18
#define NICE_FIELD_INDEX 19
#define USER_TICKS_FIELD_INDEX 13
#define KERNEL_TICKS_FIELD_INDEX 14
#else  // default is x86
#define PRIORITY_FIELD_INDEX 17
#define NICE_FIELD_INDEX 18
#define USER_TICKS_FIELD_INDEX 13
#define KERNEL_TICKS_FIELD_INDEX 14
#endif

// RAII class to automatically close FILE* on destruction
class FileCloser {
 public:
  explicit FileCloser(FILE* file) : file_(file) {
  }
  ~FileCloser() {
    if (file_) {
      fflush(file_);  // Ensure all buffered data is written
      fclose(file_);
    }
  }

  FILE* get() const {
    return file_;
  }

 private:
  FILE* file_;
};

/**
 * @brief RAII wrapper for DIR* to ensure proper closing
 */
class DirCloser {
 public:
  explicit DirCloser(DIR* dir) : dir_(dir) {
  }
  ~DirCloser() {
    if (dir_) {
      closedir(dir_);
    }
  }

  DIR* get() const {
    return dir_;
  }

 private:
  DIR* dir_;
};

// RAII class to automatically manage Unqlite database connection
class UnqliteDB {
 public:
  explicit UnqliteDB(const std::string& filename) {
    if (unqlite_open(&p_db_, filename.c_str(), UNQLITE_OPEN_CREATE) != UNQLITE_OK) {
      fprintf(stderr, "Failed to open UnQLite database: %s\n", filename.c_str());
      p_db_ = nullptr;
    }
  }

  ~UnqliteDB() {
    if (p_db_ != nullptr) {
      unqlite_close(p_db_);
      p_db_ = nullptr;
    }
  }

  bool is_open() const {
    return p_db_ != nullptr;
  }

  bool store(const std::string& key, const void* value, size_t size) {
    if (!is_open()) {
      fprintf(stderr, "Database is not open.\n");
      return false;
    }

    if (unqlite_kv_store(p_db_, key.c_str(), -1, value, size) != UNQLITE_OK) {
      fprintf(stderr, "Failed to store data in UnQLite database for key: %s\n", key.c_str());
      return false;
    }
    return true;
  }

  std::string fetch(const std::string& key) {
    if (!is_open()) {
      fprintf(stderr, "Database is not open.\n");
      return "";
    }

    unqlite_int64 n_bytes = 0;

    // Determine the size of the data to fetch
    if (unqlite_kv_fetch(p_db_, key.c_str(), -1, nullptr, &n_bytes) != UNQLITE_OK || n_bytes == 0) {
      fprintf(stderr, "Failed to fetch data size or key does not exist: %s\n", key.c_str());
      return "";  // If the key doesn't exist or data is empty, return an empty string
    }

    // Allocate a buffer to hold the data
    std::vector<char> buffer(n_bytes);
    if (unqlite_kv_fetch(p_db_, key.c_str(), -1, buffer.data(), &n_bytes) != UNQLITE_OK) {
      fprintf(stderr, "Failed to fetch data from UnQLite database for key: %s\n", key.c_str());
      return "";
    }

    return std::string(buffer.begin(), buffer.end());
  }

  bool clear() {
    if (!is_open()) {
      fprintf(stderr, "Database is not open.\n");
      return false;
    }

    int rc = unqlite_kv_delete(p_db_, nullptr, 0);
    if (rc != UNQLITE_OK) {
      fprintf(stderr, "Failed to clear the UnQLite database. Error code: %d\n", rc);
      return false;
    }

    return true;
  }

  // New method to check if a key exists
  bool key_exists(const std::string& key) {
    if (!is_open()) {
      fprintf(stderr, "Database is not open.\n");
      return false;
    }

    unqlite_int64 n_bytes = 0;
    // Attempt to fetch the data size for the key
    int rc = unqlite_kv_fetch(p_db_, key.c_str(), -1, nullptr, &n_bytes);
    if (rc == UNQLITE_OK && n_bytes > 0) {
      return true;  // Key exists
    }
    return false;  // Key does not exist or an error occurred
  }

 private:
  unqlite* p_db_ = nullptr;
};

namespace cpu_monitor {

// Atomic flag to handle graceful exit on SIGINT
std::atomic<bool> keep_running(true);

void SignalHandler(int signal) {
  if (signal == SIGINT) {
    keep_running = false;
  }
}

class ThreadIdMapper {
 public:
  explicit ThreadIdMapper(UnqliteDB& db) : db_(db) {
    if (db_.key_exists("thread_id_map")) {
      LoadThreadIdMapping();
    }
  }

  int GetOrAssignCompressedThreadId(int real_thread_id) {
    auto it = thread_id_map_.find(real_thread_id);
    if (it != thread_id_map_.end()) {
      return it->second;
    } else {
      int compressed_id = next_compressed_id_++;
      thread_id_map_[real_thread_id] = compressed_id;
      StoreThreadIdMapping();
      return compressed_id;
    }
  }

 private:
  UnqliteDB& db_;
  std::map<int, int> thread_id_map_;
  int next_compressed_id_ = 0;

  void StoreThreadIdMapping() {
    std::string key = "thread_id_map";
    std::stringstream ss;
    for (const auto& entry : thread_id_map_) {
      ss << entry.first << ":" << entry.second << ";";
    }
    std::string value = ss.str();
    fprintf(stdout, "Storing thread ID mapping: %s\n", value.c_str());
    db_.store(key, value.c_str(), value.size());
  }

  void LoadThreadIdMapping() {
    std::string key = "thread_id_map";
    std::string value = db_.fetch(key);

    if (value.empty()) {
      return;  // If there is no existing mapping, return an empty map
    }

    std::istringstream ss(value);
    std::string item;
    while (std::getline(ss, item, ';')) {
      if (!item.empty()) {
        std::istringstream pair(item);
        std::string first, second;
        if (std::getline(pair, first, ':') && std::getline(pair, second)) {
          thread_id_map_[std::stoi(first)] = std::stoi(second);
          next_compressed_id_ = std::max(next_compressed_id_, std::stoi(second) + 1);
        }
      }
    }
  }
};

class CpuUsageMonitor {
 public:
  explicit CpuUsageMonitor(int pid, int refresh_delay, const std::string& db_filename)
      : pid_(pid),
        refresh_delay_(refresh_delay),
        previous_total_cpu_time_(0),
        current_total_cpu_time_(0),
        delta_total_cpu_time_(0),
        db_(db_filename),
        thread_id_mapper_(db_) {
    InitializeThreads();  // Initialize threads
    ReadAndStoreProcessName();  // 读取并存储进程名称
    PrintProcessInfo();   // Print process and threads info
  }

  void Run() {
    GetThreadCpuTicks();
    current_total_cpu_time_ = GetTotalCpuTime();
    StoreCurrentTicksAsPrevious();

    while (cpu_monitor::keep_running) {
      std::this_thread::sleep_for(std::chrono::seconds(refresh_delay_));
      InitializeThreads();
      GetThreadCpuTicks();
      current_total_cpu_time_ = GetTotalCpuTime();
      delta_total_cpu_time_ = current_total_cpu_time_ - previous_total_cpu_time_;

      if (delta_total_cpu_time_ > 0) {
        ComputeCpuUsage();
      }

      StoreCurrentTicksAsPrevious();
    }

    fprintf(stdout, "Exiting gracefully...\n");
  }

  void StoreProcessName() {
    std::string key = "process_name";
    db_.store(key, process_name_.c_str(), process_name_.size());
  }

  void ReadAndStoreProcessName() {
    std::string comm_filename = "/proc/" + std::to_string(pid_) + "/comm";
    std::ifstream comm_file(comm_filename);
    if (comm_file.is_open()) {
      std::getline(comm_file, process_name_);
      if (!process_name_.empty()) {
        std::string key = "process_name";
        db_.store(key, process_name_.c_str(), process_name_.size());
        DEBUG_PRINT("Stored process name: %s\n", process_name_.c_str());
      } else {
        fprintf(stderr, "Process name is empty for PID %d.\n", pid_);
      }
    } else {
      fprintf(stderr, "Failed to open %s\n", comm_filename.c_str());
    }
  }

 private:
  int pid_;
  int refresh_delay_;
  std::vector<std::string> threads_;
  std::map<std::string, std::pair<int64_t, int64_t>> previous_ticks_;
  std::map<std::string, std::pair<int64_t, int64_t>> current_ticks_;
  std::map<std::string, std::string> thread_names_;
  int64_t previous_total_cpu_time_;
  int64_t current_total_cpu_time_;
  int64_t delta_total_cpu_time_;
  UnqliteDB db_;
  ThreadIdMapper thread_id_mapper_;  // Use the ThreadIdMapper instance
  std::mutex data_mutex_;
  std::map<std::string, int> thread_priorities_;  // Used to store the priority of each thread
  std::map<int, std::string> compressed_thread_names_;
  std::string process_name_;

// Define a compact structure to store CPU usage data for each thread.
// This structure is designed to minimize memory usage by using bit fields and tightly packed data.
// The use of bit fields allows the storage of small integer values with minimal space.

// Ensure 1-byte alignment for the structure to avoid padding between fields.
#pragma pack(push, 1)
  struct CompactCpuUsageData {
    uint8_t user_percent : 7;  // 7 bits for user-mode CPU usage percentage (0-100).
                               // The upper bound of 7 bits allows a maximum value of 127,
                               // but typical usage percentages are within 0-100.

    uint8_t kernel_percent : 7;  // 7 bits for kernel-mode CPU usage percentage (0-100).
                                 // Similar to user_percent, it uses 7 bits for compact storage.

    uint16_t user_ticks;  // 16 bits to store the number of CPU ticks spent in user mode.
                          // This represents the accumulated CPU time for user processes.

    uint16_t kernel_ticks;  // 16 bits to store the number of CPU ticks spent in kernel mode.
                            // This represents the accumulated CPU time for kernel processes.

    uint32_t timestamp : 20;  // 20 bits for a compressed timestamp (seconds since an epoch).
                              // The limited bit-width is used to reduce storage, typically
                              // representing time in seconds within a specific rolling window.

    uint8_t thread_id : 7;  // 7 bits to store a compressed thread identifier.
                            // This allows for 128 unique thread IDs, which is usually sufficient
                            // for most applications tracking a limited number of threads.

    uint8_t thread_status : 2;  // 2 bits for thread status, indicating the current state of the thread.
                                // Possible values could represent states such as running, sleeping,
                                // waiting, or stopped.

    uint8_t extra_flags : 3;  // 3 bits for additional flags or metadata about the thread.
                              // This could encode priority information, special states, or other
                              // thread-specific attributes.
  };
#pragma pack(pop)  // Restore the previous packing alignment.

  void InitializeThreads() {
    std::lock_guard<std::mutex> lck(data_mutex_);
    threads_.clear();
    thread_names_.clear();
    thread_priorities_.clear();
    compressed_thread_names_.clear();  // Clear compressed thread names

    std::string task_path = "/proc/" + std::to_string(pid_) + "/task";
    DIR* dir = opendir(task_path.c_str());
    if (!dir) {
      fprintf(stderr, "Failed to open directory: %s\n", task_path.c_str());
      return;
    }
    DirCloser dir_closer(dir);

    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
      std::string tid_str = ent->d_name;
      if (std::isdigit(tid_str[0]) && std::stoi(tid_str) != pid_) {
        threads_.push_back(tid_str);
        std::string comm_filename = task_path + "/" + tid_str + "/comm";
        std::ifstream comm_file(comm_filename);
        if (comm_file.is_open()) {
          std::string thread_name;
          std::getline(comm_file, thread_name);
          thread_names_[tid_str] = thread_name;

          // Get the compressed thread ID
          int real_thread_id = std::stoi(tid_str);
          int compressed_thread_id = thread_id_mapper_.GetOrAssignCompressedThreadId(real_thread_id);
          compressed_thread_names_[compressed_thread_id] = thread_name;

          // Get priority and nice values ​​from /proc/[pid]/task/[tid]/stat file
          std::string stat_filename = task_path + "/" + tid_str + "/stat";
          std::ifstream stat_file(stat_filename);
          if (!stat_file.is_open()) {
            fprintf(stderr, "Failed to open file: %s\n", stat_filename.c_str());
            continue;
          }

          std::string line;
          if (!std::getline(stat_file, line)) {
            fprintf(stderr, "Failed to read line from file: %s\n", stat_filename.c_str());
            continue;
          }

          std::istringstream iss(line);
          std::string temp;
          int priority = 0, nice_value = 0;

          // Jump to the field where the priority and nice values ​​are located (the specific field index depends on
          // the schema)
          for (int i = 0; i < PRIORITY_FIELD_INDEX; ++i) {
            if (!(iss >> temp)) {
              fprintf(stderr, "Error parsing stat file: %s\n", stat_filename.c_str());
              continue;
            }
          }

          // Get priority and nice values
          if (!(iss >> priority >> nice_value)) {
            fprintf(stderr, "Error parsing priority/nice value from stat file: %s\n", stat_filename.c_str());
            continue;
          }

          // Store priorities in a dictionary
          thread_priorities_[tid_str] = priority;
        }
      }
    }
    // Store the compressed thread names into the database
    StoreCompressedThreadNames();
  }

  void StoreCompressedThreadNames() {
    std::string key = "thread_name_map";
    std::stringstream ss;
    for (const auto& entry : compressed_thread_names_) {
      ss << entry.first << ":" << entry.second << ";";
    }
    std::string value = ss.str();
    db_.store(key, value.c_str(), value.size());
    DEBUG_PRINT("Stored thread name mapping: %s\n", value.c_str());
  }


  void GetThreadCpuTicks() {
    std::lock_guard<std::mutex> lck(data_mutex_);
    current_ticks_.clear();
    for (const auto& thread : threads_) {
      std::string stat_filename = "/proc/" + std::to_string(pid_) + "/task/" + thread + "/stat";
      auto thread_data = ParseStatFile(stat_filename);
      if (thread_data.size() == 2) {
        current_ticks_[thread] = {thread_data[0], thread_data[1]};
      }
    }
  }

  void ComputeCpuUsage() {
    std::lock_guard<std::mutex> lck(data_mutex_);

    auto now = std::chrono::system_clock::now();
    auto time_since_start =
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch() % std::chrono::seconds(259200))
            .count();  // 259200 seconds = 3 days
    uint32_t compressed_timestamp = static_cast<uint32_t>(time_since_start);

    std::vector<CompactCpuUsageData> batch_data;

    for (const auto& thread : threads_) {
      int64_t user_delta = 0;
      int64_t kernel_delta = 0;

      if (previous_ticks_.find(thread) != previous_ticks_.end()) {
        user_delta = current_ticks_.at(thread).first - previous_ticks_.at(thread).first;
        kernel_delta = current_ticks_.at(thread).second - previous_ticks_.at(thread).second;
      }

      uint32_t user_percent = 0;
      uint32_t kernel_percent = 0;

      if (delta_total_cpu_time_ > 0) {
        user_percent = static_cast<uint32_t>(static_cast<double>(user_delta) / delta_total_cpu_time_ * 100.0);
        kernel_percent = static_cast<uint32_t>(static_cast<double>(kernel_delta) / delta_total_cpu_time_ * 100.0);
      }

      uint32_t thread_status = 0;  // eg. 0 = running, 1 = sleeping, 2 = waiting, 3 = stopped
      uint32_t extra_flags = 0;    // eg. priority or other flags can be stored

      if (thread_names_[thread].find("worker") != std::string::npos) {
        thread_status = 1;  // Assuming a thread with the name 'worker' is in sleep mode
      }
      extra_flags = thread_priorities_[thread] & 0xF;  // Only take the lower 4 bits of priority as additional flags

      int real_thread_id = std::stoi(thread);
      int compressed_thread_id = thread_id_mapper_.GetOrAssignCompressedThreadId(real_thread_id);

      CompactCpuUsageData data;
      data.user_percent = user_percent & 0x7F;
      data.kernel_percent = kernel_percent & 0x7F;
      data.user_ticks = static_cast<uint16_t>(current_ticks_.at(thread).first);
      data.kernel_ticks = static_cast<uint16_t>(current_ticks_.at(thread).second);
      data.timestamp = compressed_timestamp & 0xFFFFF;
      data.thread_id = compressed_thread_id & 0x7F;
      data.thread_status = thread_status & 0x03;
      data.extra_flags = extra_flags & 0x07;

      batch_data.push_back(data);

      DEBUG_PRINT("Thread %u: user_percent=%u, kernel_percent=%u, status=%u, flags=%u\n", data.thread_id, user_percent,
                  kernel_percent, thread_status, extra_flags);
    }

    std::string collection_key = "batch_" + std::to_string(compressed_timestamp);
    db_.store(collection_key, batch_data.data(), batch_data.size() * sizeof(CompactCpuUsageData));
  }

  int64_t GetTotalCpuTime() const {
    std::ifstream file("/proc/stat");
    if (!file.is_open()) {
      fprintf(stderr, "Failed to open /proc/stat\n");
      return 0;
    }

    std::string line;
    if (!std::getline(file, line)) {
      fprintf(stderr, "Failed to read line from /proc/stat\n");
      return 0;
    }

    std::istringstream iss(line);
    std::string temp;
    int64_t user, nice, system, idle, iowait, irq, softirq;
    if (!(iss >> temp >> user >> nice >> system >> idle >> iowait >> irq >> softirq)) {
      fprintf(stderr, "Error parsing /proc/stat\n");
      return 0;
    }

    DEBUG_PRINT("Total CPU time: user=%ld, nice=%ld, system=%ld, idle=%ld, iowait=%ld, irq=%ld, softirq=%ld\n", user,
                nice, system, idle, iowait, irq, softirq);

    return user + nice + system + idle + iowait + irq + softirq;
  }

  void StoreCurrentTicksAsPrevious() {
    std::lock_guard<std::mutex> lck(data_mutex_);
    previous_ticks_ = current_ticks_;
    previous_total_cpu_time_ = current_total_cpu_time_;
  }

  std::vector<int64_t> ParseStatFile(const std::string& filename) const {
    std::ifstream file(filename);
    if (!file.is_open()) {
      fprintf(stderr, "Failed to open file: %s\n", filename.c_str());
      return {};
    }

    std::string line;
    if (!std::getline(file, line)) {
      fprintf(stderr, "Failed to read line from file: %s\n", filename.c_str());
      return {};
    }

    std::istringstream iss(line);
    std::vector<int64_t> values;
    std::string temp;

    for (int i = 0; i < USER_TICKS_FIELD_INDEX; ++i) {
      if (!(iss >> temp)) {
        fprintf(stderr, "Error parsing stat file: %s\n", filename.c_str());
        return {};
      }
    }

    int64_t user_time = 0;
    int64_t kernel_time = 0;

    if (!(iss >> user_time)) {
      fprintf(stderr, "Error parsing user_time from stat file: %s\n", filename.c_str());
      return {};
    }

    if (!(iss >> kernel_time)) {
      fprintf(stderr, "Error parsing kernel_time from stat file: %s\n", filename.c_str());
      return {};
    }

    values.push_back(user_time);
    values.push_back(kernel_time);

    return values;
  }

  void PrintProcessInfo() {
    int process_priority = getpriority(PRIO_PROCESS, pid_);
    if (process_priority == -1 && errno != 0) {
      fprintf(stderr, "Failed to get process priority: %s\n", strerror(errno));
    } else {
      fprintf(stdout, "Process ID: %d\n", pid_);
      fprintf(stdout, "Process Priority: %d\n", process_priority);
    }
  }
};

}  // namespace cpu_monitor

bool FindPidByProcessName(const std::string& process_name, pid_t& pid) {  // NOLINT
  DirCloser dir(opendir("/proc"));
  if (!dir.get()) {
    fprintf(stderr, "Failed to open directory /proc\n");
    return false;
  }

  struct dirent* entry;
  while ((entry = readdir(dir.get())) != nullptr) {
    // Check if the entry is a directory and its name is numeric
    if (entry->d_type == DT_DIR) {
      std::string pid_str = entry->d_name;
      if (pid_str.find_first_not_of("0123456789") == std::string::npos) {
        // Read the "cmdline" file to get the process name
        std::string cmdline_path = "/proc/" + pid_str + "/cmdline";
        FILE* cmdline_file = fopen(cmdline_path.c_str(), "r");
        if (cmdline_file) {
          FileCloser file_closer(cmdline_file);

          std::stringstream cmdline_stream;
          char buffer[256];
          while (fgets(buffer, sizeof(buffer), cmdline_file) != nullptr) {
            cmdline_stream << buffer;
          }

          std::string cmdLine = cmdline_stream.str();
          // Check if the process name matches
          if (cmdLine.find(process_name) != std::string::npos) {
            pid = std::stoi(pid_str);
            // just return the first pid found
            return true;
          }
        } else {
          fprintf(stderr, "Failed to open %s\n", cmdline_path.c_str());
        }
      }
    }
  }

  return false;
}

bool ParseCommandLineArgs(int argc, char* argv[], std::vector<int>& pids, int& refresh_delay,
                          std::string& db_filename) {
  int opt;
  while ((opt = getopt(argc, argv, "hn:d:")) != -1) {
    switch (opt) {
      case 'h':
        fprintf(stdout, "Usage: %s [options] <pid_or_process_name>...\n", argv[0]);
        fprintf(stdout, "Options:\n");
        fprintf(stdout, " -h Display help\n");
        fprintf(stdout, " -n <delay> Set the display refresh value in sec.\n");
        fprintf(stdout, " -d <filename> Set the UnQLite database file name.\n");
        return false;
      case 'n':
        refresh_delay = std::stoi(optarg);
        if (refresh_delay <= 0) {
          fprintf(stderr, "Refresh delay must be a positive integer.\n");
          return false;
        }
        break;
      case 'd':
        db_filename = optarg;
        break;
      default:
        fprintf(stderr, "Usage: %s [options] <pid_or_process_name>...\n", argv[0]);
        return false;
    }
  }

  for (int i = optind; i < argc; ++i) {
    std::string pid_or_name = argv[i];
    int pid = -1;

    if (pid_or_name.find_first_not_of("0123456789") == std::string::npos) {
      pid = std::stoi(pid_or_name);
    } else {
      if (!FindPidByProcessName(pid_or_name, pid)) {
        fprintf(stderr, "Failed to find process with name: %s\n", pid_or_name.c_str());
        return false;
      }
    }

    pids.push_back(pid);
  }

  if (pids.empty()) {
    fprintf(stderr, "Missing PID or process name.\n");
    return false;
  }

  return true;
}

int main(int argc, char* argv[]) {
  std::signal(SIGINT, cpu_monitor::SignalHandler);

  std::vector<int> pids;
  int refresh_delay = 2;
  std::string db_filename{"unqlite.db"};

  if (!ParseCommandLineArgs(argc, argv, pids, refresh_delay, db_filename)) {
    return 1;
  }

  if (access(db_filename.c_str(), F_OK) == 0) {     // Check if the file exists
    if (access(db_filename.c_str(), W_OK) == 0) {   // Check if the file is writable
      if (std::remove(db_filename.c_str()) != 0) {  // Attempt to delete the file
        perror("Failed to remove existing database file");
        return 1;
      } else {
        fprintf(stderr, "Removed existing database file '%s'.\n", db_filename.c_str());
      }
    } else {
      fprintf(stderr, "Existing database file '%s' is not writable.\n", db_filename.c_str());
      return 1;
    }
  }

  std::vector<std::thread> monitor_threads;
  for (const int& pid : pids) {
    monitor_threads.emplace_back([pid, refresh_delay, &db_filename]() {
      try {
        cpu_monitor::CpuUsageMonitor monitor(pid, refresh_delay, db_filename);
        monitor.Run();
      } catch (const std::exception& e) {
        fprintf(stderr, "Exception caught in monitor thread: %s\n", e.what());
      }
    });
  }

  for (auto& thread : monitor_threads) {
    thread.join();
  }

  return 0;
}
