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

// Define macro to control debug output, default is disabled
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
#else // Default is x86
#define PRIORITY_FIELD_INDEX 17
#define NICE_FIELD_INDEX 18
#define USER_TICKS_FIELD_INDEX 13
#define KERNEL_TICKS_FIELD_INDEX 14
#endif

// RAII class to automatically close FILE*
class FileCloser {
public:
  explicit FileCloser(FILE *file) : file_(file) {}
  ~FileCloser() {
    if (file_) {
      fflush(file_);
      fclose(file_);
    }
  }
  FILE *get() const { return file_; }

private:
  FILE *file_;
};

// RAII class to automatically close DIR*
class DirCloser {
public:
  explicit DirCloser(DIR *dir) : dir_(dir) {}
  ~DirCloser() {
    if (dir_) {
      closedir(dir_);
    }
  }
  DIR *get() const { return dir_; }

private:
  DIR *dir_;
};

namespace cpu_monitor {

// Atomic flag to gracefully exit on SIGINT
std::atomic<bool> keep_running(true);

// Signal handler to set the atomic flag
void SignalHandler(int signal) {
  if (signal == SIGINT) {
    keep_running = false;
  }
}

// Struct to store CPU usage data without compression
#pragma pack(push, 1)
struct CpuUsageData {
  uint8_t user_percent;   // 0-100
  uint8_t kernel_percent; // 0-100
  uint16_t user_ticks;    // User mode CPU ticks
  uint16_t kernel_ticks;  // Kernel mode CPU ticks
  uint32_t timestamp;     // Timestamp in seconds since epoch
  uint32_t thread_id;     // Full thread ID
  uint8_t thread_status;  // Thread status, 2 bits
  uint8_t extra_flags;    // Extra flags, 3 bits
};
#pragma pack(pop)

class CpuUsageMonitor {
public:
  CpuUsageMonitor(int pid, double refresh_delay,
                  const std::string &output_filename)
      : pid_(pid), refresh_delay_(refresh_delay),
        output_filename_(output_filename), previous_total_cpu_time_(0),
        current_total_cpu_time_(0), delta_total_cpu_time_(0) {
    InitializeThreads();
    ReadAndStoreProcessName();
    WriteFileHeader(); // Write header to binary file
    PrintProcessInfo();
  }

  void Run() {
    GetThreadCpuTicks();
    current_total_cpu_time_ = GetTotalCpuTime();
    StoreCurrentTicksAsPrevious();

    while (keep_running) {
      std::this_thread::sleep_for(
          std::chrono::duration<double>(refresh_delay_));
      InitializeThreads();
      GetThreadCpuTicks();
      current_total_cpu_time_ = GetTotalCpuTime();
      delta_total_cpu_time_ =
          current_total_cpu_time_ - previous_total_cpu_time_;

      if (delta_total_cpu_time_ > 0) {
        ComputeCpuUsage();
      }

      StoreCurrentTicksAsPrevious();
    }

    fprintf(stdout, "Exiting gracefully...\n");
  }

private:
  int pid_;
  double refresh_delay_;
  std::string output_filename_;
  std::vector<std::string> threads_;
  std::map<std::string, std::pair<int64_t, int64_t>> previous_ticks_;
  std::map<std::string, std::pair<int64_t, int64_t>> current_ticks_;
  std::map<std::string, std::string> thread_names_;
  int64_t previous_total_cpu_time_;
  int64_t current_total_cpu_time_;
  int64_t delta_total_cpu_time_;
  std::mutex data_mutex_;
  std::map<std::string, int> thread_priorities_;
  std::string process_name_;

  // Function to write the header to the binary file
  void WriteFileHeader() {
    std::ofstream binary_file(output_filename_,
                              std::ios::binary | std::ios::out);
    if (!binary_file.is_open()) {
      fprintf(stderr, "Failed to open %s for writing header.\n",
              output_filename_.c_str());
      return;
    }

    // Serialize the header
    std::stringstream ss;

    // Placeholder for header_size (4 bytes)
    uint32_t header_size = 0;
    ss.write(reinterpret_cast<const char *>(&header_size), sizeof(header_size));

    // Write process_name length and content
    uint32_t process_name_length = process_name_.size();
    ss.write(reinterpret_cast<const char *>(&process_name_length),
             sizeof(process_name_length));
    ss.write(process_name_.c_str(), process_name_length);

    // Write thread_name_map size
    uint32_t thread_map_size = thread_names_.size();
    ss.write(reinterpret_cast<const char *>(&thread_map_size),
             sizeof(thread_map_size));

    // Write each thread's thread_id and thread_name
    for (const auto &entry : thread_names_) {
      uint32_t thread_id = static_cast<uint32_t>(std::stoul(entry.first));
      ss.write(reinterpret_cast<const char *>(&thread_id), sizeof(thread_id));

      uint32_t thread_name_length = entry.second.size();
      ss.write(reinterpret_cast<const char *>(&thread_name_length),
               sizeof(thread_name_length));
      ss.write(entry.second.c_str(), thread_name_length);
    }

    // Calculate header_size (excluding the first 4 bytes)
    header_size = static_cast<uint32_t>(ss.tellp()) - sizeof(header_size);

    // Seek to the beginning and write the actual header_size
    ss.seekp(0, std::ios::beg);
    ss.write(reinterpret_cast<const char *>(&header_size), sizeof(header_size));

    // Write the header to the binary file
    std::string header_str = ss.str();
    binary_file.write(header_str.c_str(), header_str.size());

    // Debug: Print thread mappings
    DEBUG_PRINT("Writing Header:\n");
    DEBUG_PRINT("Process Name: %s\n", process_name_.c_str());
    for (const auto &entry : thread_names_) {
      DEBUG_PRINT("Thread ID: %s, Thread Name: %s\n", entry.first.c_str(),
                  entry.second.c_str());
    }

    binary_file.close();
  }

  // Function to write CPU usage data to binary file in append mode
  void WriteDataToBinaryFile(const std::vector<CpuUsageData> &data) {
    std::ofstream binary_file(output_filename_,
                              std::ios::binary | std::ios::app);
    if (!binary_file.is_open()) {
      fprintf(stderr, "Failed to open %s for writing.\n",
              output_filename_.c_str());
      return;
    }

    binary_file.write(reinterpret_cast<const char *>(data.data()),
                      data.size() * sizeof(CpuUsageData));

    binary_file.close();
  }

  // Initialize threads and populate thread_names_
  void InitializeThreads() {
    std::lock_guard<std::mutex> lck(data_mutex_);
    threads_.clear();
    thread_names_.clear();
    thread_priorities_.clear();

    std::string task_path = "/proc/" + std::to_string(pid_) + "/task";
    DIR *dir = opendir(task_path.c_str());
    if (!dir) {
      fprintf(stderr, "Failed to open directory: %s\n", task_path.c_str());
      return;
    }
    DirCloser dir_closer(dir);

    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
      std::string tid_str = ent->d_name;
      if (std::isdigit(tid_str[0])) {
        threads_.push_back(tid_str);
        std::string comm_filename = task_path + "/" + tid_str + "/comm";
        std::ifstream comm_file(comm_filename);
        if (comm_file.is_open()) {
          std::string thread_name;
          if (std::getline(comm_file, thread_name)) {
            thread_names_[tid_str] = thread_name;
          } else {
            fprintf(stderr, "Failed to read thread name for TID %s\n",
                    tid_str.c_str());
            thread_names_[tid_str] = "unknown";
          }

          // Get thread priority and nice value
          std::string stat_filename = task_path + "/" + tid_str + "/stat";
          std::ifstream stat_file(stat_filename);
          if (!stat_file.is_open()) {
            fprintf(stderr, "Failed to open file: %s\n", stat_filename.c_str());
            continue;
          }

          std::string line;
          if (!std::getline(stat_file, line)) {
            fprintf(stderr, "Failed to read line from file: %s\n",
                    stat_filename.c_str());
            continue;
          }

          std::istringstream iss(line);
          std::string temp;
          int priority = 0, nice_value = 0;

          // Skip to the priority and nice fields
          for (int i = 0; i < PRIORITY_FIELD_INDEX; ++i) {
            if (!(iss >> temp)) {
              fprintf(stderr, "Error parsing stat file: %s\n",
                      stat_filename.c_str());
              continue;
            }
          }

          // Read priority and nice value
          if (!(iss >> priority >> nice_value)) {
            fprintf(stderr,
                    "Error parsing priority/nice value from stat file: %s\n",
                    stat_filename.c_str());
            continue;
          }

          // Store priority
          thread_priorities_[tid_str] = priority;

        } else {
          fprintf(stderr, "Failed to open comm file for TID %s\n",
                  tid_str.c_str());
          thread_names_[tid_str] = "unknown";
        }
      }
    }
  }

  // Function to get CPU ticks for each thread
  void GetThreadCpuTicks() {
    std::lock_guard<std::mutex> lck(data_mutex_);
    current_ticks_.clear();
    for (const auto &thread : threads_) {
      std::string stat_filename =
          "/proc/" + std::to_string(pid_) + "/task/" + thread + "/stat";
      auto thread_data = ParseStatFile(stat_filename);
      if (thread_data.size() == 2) {
        current_ticks_[thread] = {thread_data[0], thread_data[1]};
      }
    }
  }

  // Function to compute CPU usage and write to binary file
  void ComputeCpuUsage() {
    std::lock_guard<std::mutex> lck(data_mutex_);

    auto now = std::chrono::system_clock::now();
    auto epoch = now.time_since_epoch();
    auto seconds_since_epoch =
        std::chrono::duration_cast<std::chrono::seconds>(epoch).count();
    uint32_t timestamp = static_cast<uint32_t>(seconds_since_epoch);

    std::vector<CpuUsageData> batch_data;

    for (const auto &thread : threads_) {
      int64_t user_delta = 0;
      int64_t kernel_delta = 0;

      if (previous_ticks_.find(thread) != previous_ticks_.end()) {
        user_delta =
            current_ticks_.at(thread).first - previous_ticks_.at(thread).first;
        kernel_delta = current_ticks_.at(thread).second -
                       previous_ticks_.at(thread).second;
      }

      uint32_t user_percent = 0;
      uint32_t kernel_percent = 0;

      if (delta_total_cpu_time_ > 0) {
        user_percent = static_cast<uint32_t>(static_cast<double>(user_delta) /
                                             delta_total_cpu_time_ * 100.0);
        kernel_percent = static_cast<uint32_t>(
            static_cast<double>(kernel_delta) / delta_total_cpu_time_ * 100.0);
      }

      uint8_t thread_status =
          0; // e.g., 0 = Running, 1 = Sleeping, 2 = Waiting, 3 = Stopped
      uint8_t extra_flags = 0; // Can store priority or other flags

      auto it = thread_names_.find(thread);
      std::string thread_name =
          (it != thread_names_.end()) ? it->second : "unknown";

      if (thread_name.find("worker") != std::string::npos) {
        thread_status = 1; // Assume threads with 'worker' in name are sleeping
      }
      extra_flags = thread_priorities_[thread] &
                    0x7; // Lower 3 bits of priority as extra flags

      int real_thread_id = std::stoi(thread);

      CpuUsageData data;
      data.user_percent =
          user_percent > 100 ? 100 : static_cast<uint8_t>(user_percent);
      data.kernel_percent =
          kernel_percent > 100 ? 100 : static_cast<uint8_t>(kernel_percent);
      data.user_ticks = static_cast<uint16_t>(current_ticks_.at(thread).first);
      data.kernel_ticks =
          static_cast<uint16_t>(current_ticks_.at(thread).second);
      data.timestamp = timestamp;
      data.thread_id = static_cast<uint32_t>(real_thread_id);
      data.thread_status = thread_status;
      data.extra_flags = extra_flags;

      batch_data.push_back(data);

      DEBUG_PRINT("Thread ID %u: user_percent=%u, kernel_percent=%u, "
                  "status=%u, flags=%u\n",
                  data.thread_id, data.user_percent, data.kernel_percent,
                  data.thread_status, data.extra_flags);
    }

    // Write data to binary file
    WriteDataToBinaryFile(batch_data);
  }

  // Function to get total CPU time from /proc/stat
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
    if (!(iss >> temp >> user >> nice >> system >> idle >> iowait >> irq >>
          softirq)) {
      fprintf(stderr, "Error parsing /proc/stat\n");
      return 0;
    }

    DEBUG_PRINT("Total CPU time: user=%ld, nice=%ld, system=%ld, idle=%ld, "
                "iowait=%ld, irq=%ld, softirq=%ld\n",
                user, nice, system, idle, iowait, irq, softirq);

    return user + nice + system + idle + iowait + irq + softirq;
  }

  // Function to store current ticks as previous ticks
  void StoreCurrentTicksAsPrevious() {
    std::lock_guard<std::mutex> lck(data_mutex_);
    previous_ticks_ = current_ticks_;
    previous_total_cpu_time_ = current_total_cpu_time_;
  }

  // Function to parse /proc/[pid]/task/[tid]/stat file
  std::vector<int64_t> ParseStatFile(const std::string &filename) const {
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
      fprintf(stderr, "Error parsing user_time from stat file: %s\n",
              filename.c_str());
      return {};
    }

    if (!(iss >> kernel_time)) {
      fprintf(stderr, "Error parsing kernel_time from stat file: %s\n",
              filename.c_str());
      return {};
    }

    values.push_back(user_time);
    values.push_back(kernel_time);

    return values;
  }

  // Function to read and store process name
  void ReadAndStoreProcessName() {
    std::string comm_filename = "/proc/" + std::to_string(pid_) + "/comm";
    std::ifstream comm_file(comm_filename);
    if (comm_file.is_open()) {
      std::getline(comm_file, process_name_);
      if (!process_name_.empty()) {
        DEBUG_PRINT("Process name: %s\n", process_name_.c_str());
      } else {
        fprintf(stderr, "Process name is empty for PID %d.\n", pid_);
      }
    } else {
      fprintf(stderr, "Failed to open %s\n", comm_filename.c_str());
    }
  }

  // Function to print process info and all threads with their priorities
  void PrintProcessInfo() {
    // Lock the mutex to ensure thread-safe access to shared data
    std::lock_guard<std::mutex> lck(data_mutex_);

    // Retrieve and print process priority
    int process_priority = getpriority(PRIO_PROCESS, pid_);
    if (process_priority == -1 && errno != 0) {
      fprintf(stderr, "Failed to get process priority: %s\n", strerror(errno));
    } else {
      fprintf(stdout, "Process Name: %s\n", process_name_.c_str());
      fprintf(stdout, "Process ID: %d\n", pid_);
      fprintf(stdout, "Process Priority: %d\n", process_priority);
    }

    // Print thread information
    fprintf(stdout, "Threads (%zu):\n", thread_names_.size());
    for (const auto &entry : thread_names_) {
      // Get thread ID and name
      const std::string &thread_id_str = entry.first;
      const std::string &thread_name = entry.second;

      // Retrieve thread priority
      auto priority_it = thread_priorities_.find(thread_id_str);
      if (priority_it != thread_priorities_.end()) {
        int thread_priority = priority_it->second;
        fprintf(stdout, "  Thread Name: %s, Thread ID: %s,  Priority: %d\n",
                thread_name.c_str(), thread_id_str.c_str(), thread_priority);
      } else {
        fprintf(stdout,
                "  Thread Name: %s, Thread ID: %s,  Priority: Unknown\n",
                thread_name.c_str(), thread_id_str.c_str());
      }
    }
  }
};

// Function to find PID by process name
bool FindPidByProcessName(const std::string &process_name, pid_t &pid) {
  DIR *dir = opendir("/proc");
  if (!dir) {
    fprintf(stderr, "Failed to open directory /proc\n");
    return false;
  }
  DirCloser dir_closer(dir);

  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (entry->d_type == DT_DIR) {
      std::string pid_str = entry->d_name;
      if (pid_str.find_first_not_of("0123456789") == std::string::npos) {
        std::string cmdline_path = "/proc/" + pid_str + "/cmdline";
        FILE *cmdline_file = fopen(cmdline_path.c_str(), "r");
        if (cmdline_file) {
          FileCloser file_closer(cmdline_file);

          std::stringstream cmdline_stream;
          char buffer[256];
          while (fgets(buffer, sizeof(buffer), cmdline_file) != nullptr) {
            cmdline_stream << buffer;
          }

          std::string cmdLine = cmdline_stream.str();
          if (cmdLine.find(process_name) != std::string::npos) {
            pid = std::stoi(pid_str);
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

// Function to parse command line arguments
bool ParseCommandLineArgs(int argc, char *argv[], int &pid,
                          double &refresh_delay, std::string &output_filename) {
  int opt;
  output_filename = "cpu_usage.bin"; // Default output filename

  while ((opt = getopt(argc, argv, "hn:o:")) != -1) {
    switch (opt) {
    case 'h':
      fprintf(stdout, "Usage: %s [options] <pid_or_process_name>\n", argv[0]);
      fprintf(stdout, "Options:\n");
      fprintf(stdout, " -h Display help\n");
      fprintf(stdout, " -n <delay> Set the CPU usage refresh interval in "
                      "seconds (supports decimals, e.g., 0.1).\n");
      fprintf(stdout, " -o <output_file> Set the output binary filename.\n");
      return false;
    case 'n':
      try {
        refresh_delay = std::stod(optarg);
        if (refresh_delay <= 0.0) {
          fprintf(stderr, "Refresh delay must be a positive number.\n");
          return false;
        }
      } catch (const std::invalid_argument &) {
        fprintf(stderr, "Invalid refresh delay value: %s\n", optarg);
        return false;
      }
      break;
    case 'o':
      output_filename = optarg;
      break;
    default:
      fprintf(stderr, "Usage: %s [options] <pid_or_process_name>\n", argv[0]);
      return false;
    }
  }

  if (optind < argc) {
    std::string pid_or_name = argv[optind];
    if (pid_or_name.find_first_not_of("0123456789") == std::string::npos) {
      pid = std::stoi(pid_or_name);
    } else {
      if (!FindPidByProcessName(pid_or_name, pid)) {
        fprintf(stderr, "Failed to find process with name: %s\n",
                pid_or_name.c_str());
        return false;
      }
    }
  } else {
    fprintf(stderr, "Missing PID or process name.\n");
    return false;
  }

  return true;
}

} // namespace cpu_monitor

int main(int argc, char *argv[]) {
  std::signal(SIGINT, cpu_monitor::SignalHandler);

  int pid = -1;
  double refresh_delay = 2.0; // Default refresh delay in seconds
  std::string output_filename = "cpu_usage.bin"; // Default output filename

  if (!cpu_monitor::ParseCommandLineArgs(argc, argv, pid, refresh_delay,
                                         output_filename)) {
    return 1;
  }

  try {
    cpu_monitor::CpuUsageMonitor monitor(pid, refresh_delay, output_filename);
    monitor.Run();
  } catch (const std::exception &e) {
    fprintf(stderr, "Exception caught in monitor: %s\n", e.what());
    return 1;
  }

  return 0;
}
