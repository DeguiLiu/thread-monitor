#include <dirent.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
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
#define DEBUG_PRINT(fmt, ...) ((void)0)
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
    if (file_ != nullptr) {
      (void)fflush(file_);
      (void)fclose(file_);
    }
  }
  FILE *get() const { return file_; }

private:
  FILE *file_;
  // Disallow copying
  FileCloser(const FileCloser &) = delete;
  FileCloser &operator=(const FileCloser &) = delete;
};

// RAII class to automatically close DIR*
class DirCloser {
public:
  explicit DirCloser(DIR *dir) : dir_(dir) {}
  ~DirCloser() {
    if (dir_ != nullptr) {
      (void)closedir(dir_);
    }
  }
  DIR *get() const { return dir_; }

private:
  DIR *dir_;
  // Disallow copying
  DirCloser(const DirCloser &) = delete;
  DirCloser &operator=(const DirCloser &) = delete;
};

namespace cpu_monitor {

// Atomic flag to gracefully exit on SIGINT
std::atomic<bool> keep_running(true);

// Signal handler to set the atomic flag
void SignalHandler(int signal) {
  if (signal == SIGINT) {
    keep_running.store(false);
  }
}

// Struct to store CPU usage data without compression
#pragma pack(push, 1)
struct CpuUsageData {
  uint16_t user_percent;   // 0-10000, 10000 = 100%
  uint16_t kernel_percent; // 0-10000, 10000 = 100%
  uint32_t user_ticks;     // User CPU ticks
  uint32_t kernel_ticks;   // Kernel CPU ticks
  uint32_t timestamp;      // Timestamp in seconds since epoch
  uint32_t thread_id;      // Thread ID
  uint8_t thread_status;   // Thread status
  uint8_t extra_flags;     // Extra flags
};
#pragma pack(pop)

class CpuUsageMonitor {
public:
  CpuUsageMonitor(int pid, double refresh_delay, double total_duration,
                  const std::string &output_filename)
      : pid_(pid), refresh_delay_(refresh_delay),
        total_duration_(total_duration), output_filename_(output_filename) {
    ticks_per_second_ = static_cast<double>(sysconf(_SC_CLK_TCK));
    num_cpus_ = std::thread::hardware_concurrency();
    InitializeThreads();
    ReadAndStoreProcessName();
    WriteFileHeader(); // Write header to binary file
    PrintProcessInfo();
  }

  void Run() {
    int iteration = 0;
    const int kUpdateInterval = 5; // Update the thread list every 5 iterations
    GetThreadCpuTicks();
    current_total_cpu_time_ = GetTotalCpuTime();
    StoreCurrentTicksAsPrevious();

    auto start_time = std::chrono::steady_clock::now();
    fprintf(stdout, "Monitoring CPU usage for process %s (pid=%d)\n",
            process_name_.c_str(), pid_);

    while (keep_running) {
      fprintf(stdout, ".");
      std::this_thread::sleep_for(
          std::chrono::duration<double>(refresh_delay_));

      // Check if total duration has been reached
      if (total_duration_ > 0) {
        auto elapsed_time = std::chrono::steady_clock::now() - start_time;
        if (elapsed_time >= std::chrono::duration<double>(total_duration_)) {
          break;
        }
      }

      if (iteration % kUpdateInterval == 0) {
        InitializeThreads(); // Regularly update the thread list
      }
      GetThreadCpuTicks();
      current_total_cpu_time_ = GetTotalCpuTime();
      delta_total_cpu_time_ =
          current_total_cpu_time_ - previous_total_cpu_time_;

      if (delta_total_cpu_time_ > 0.0) {
        ComputeCpuUsage();
      }

      StoreCurrentTicksAsPrevious();
    }

    fprintf(stdout, "Exiting gracefully...\n");
  }

private:
  int pid_;
  double refresh_delay_{0.0};
  double total_duration_{0.0};
  std::string output_filename_;
  std::vector<std::string> threads_;
  std::map<std::string, std::pair<uint64_t, uint64_t>> previous_ticks_;
  std::map<std::string, std::pair<uint64_t, uint64_t>> current_ticks_;
  std::map<std::string, std::string> thread_names_;
  double previous_total_cpu_time_{0.0};
  double current_total_cpu_time_{0.0};
  double delta_total_cpu_time_{0.0};
  std::mutex data_mutex_;
  std::map<std::string, int> thread_priorities_;
  std::string process_name_;
  double ticks_per_second_{0.0};
  int num_cpus_{0};

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
    uint32_t header_size = 0U;
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
    binary_file.close();

#if ENABLE_DEBUG_OUTPUT
    // Debug: Print thread mappings
    DEBUG_PRINT("%s\n", "Writing Header:");
    DEBUG_PRINT("Process Name: %s\n", process_name_.c_str());
    for (const auto &entry : thread_names_) {
      DEBUG_PRINT("thread_id: %s, thread_name: %s\n", entry.first.c_str(),
                  entry.second.c_str());
    }
#endif
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

      // Catalog of '.' And '..'
      if (tid_str == "." || tid_str == "..") {
        continue;
      }

      // Check whether the directory name is numbers (thread_id)
      if (std::all_of(tid_str.begin(), tid_str.end(),
                      [](unsigned char c) { return std::isdigit(c); })) {
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

          // Get the priority and NICE value of the thread
          const std::string stat_filename = task_path + "/" + tid_str + "/stat";
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

          // Analyze the content of the stat file
          // The second field may contain the space in the bracket, and it needs
          // to be dealt with properly
          size_t first_paren = line.find('(');
          size_t last_paren = line.rfind(')');
          if (first_paren == std::string::npos ||
              last_paren == std::string::npos || last_paren <= first_paren) {
            fprintf(stderr, "Malformed stat file: %s\n", stat_filename.c_str());
            continue;
          }

          std::string before_paren = line.substr(0, first_paren);
          std::string between_paren =
              line.substr(first_paren + 1, last_paren - first_paren - 1);
          std::string after_paren = line.substr(last_paren + 1);

          std::istringstream iss(after_paren);
          std::vector<std::string> fields;
          std::string field;

          // The first two fields have been processed
          fields.push_back(before_paren);
          fields.push_back(between_paren);

          while (iss >> field) {
            fields.push_back(field);
          }

          if (fields.size() <= NICE_FIELD_INDEX) {
            fprintf(stderr, "Not enough fields in stat file: %s\n",
                    stat_filename.c_str());
            continue;
          }

          int priority = std::stoi(fields[PRIORITY_FIELD_INDEX]);

          // Store priority
          thread_priorities_[tid_str] = priority;

        } else {
          fprintf(stderr, "Failed to open comm file for TID %s\n",
                  tid_str.c_str());
          thread_names_[tid_str] = "unknown";
        }
      } else {
        DEBUG_PRINT("Skipping non-numeric entry: %s\n", tid_str.c_str());
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

    double total_time = delta_total_cpu_time_;

    for (const auto &thread : threads_) {
      uint64_t user_delta = 0;
      uint64_t kernel_delta = 0;

      if (previous_ticks_.find(thread) != previous_ticks_.end()) {
        user_delta =
            current_ticks_.at(thread).first - previous_ticks_.at(thread).first;
        kernel_delta = current_ticks_.at(thread).second -
                       previous_ticks_.at(thread).second;
      }

      double user_time_seconds =
          static_cast<double>(user_delta) / ticks_per_second_;
      double kernel_time_seconds =
          static_cast<double>(kernel_delta) / ticks_per_second_;

      uint16_t user_percent = 0;   // 0-10000
      uint16_t kernel_percent = 0; // 0-10000

      if (total_time > 0) {
        user_percent =
            static_cast<uint16_t>(user_time_seconds / total_time * 10000.0);
        kernel_percent =
            static_cast<uint16_t>(kernel_time_seconds / total_time * 10000.0);
      }

      if (user_percent > 10000)
        user_percent = 10000;
      if (kernel_percent > 10000)
        kernel_percent = 10000;

      uint8_t thread_status =
          0; // e.g., 0 = Running, 1 = Sleeping, 2 = Waiting, 3 = Stopped
      uint8_t extra_flags = 0; // Can store priority or other flags

      auto it = thread_names_.find(thread);
      std::string thread_name =
          (it != thread_names_.end()) ? it->second : "unknown";

      if (thread_name.find("worker") != std::string::npos) {
        thread_status = 1; // Assume threads with 'worker' in name are sleeping
      }
      extra_flags = static_cast<uint8_t>(thread_priorities_[thread]);

      int real_thread_id = std::stoi(thread);

      CpuUsageData data;
      data.user_percent = static_cast<uint16_t>(user_percent);
      data.kernel_percent = static_cast<uint16_t>(kernel_percent);
      data.user_ticks = static_cast<uint32_t>(current_ticks_.at(thread).first);
      data.kernel_ticks =
          static_cast<uint32_t>(current_ticks_.at(thread).second);
      data.timestamp = timestamp;
      data.thread_id = static_cast<uint32_t>(real_thread_id);
      data.thread_status = thread_status;
      data.extra_flags = extra_flags;

      batch_data.push_back(data);

#if ENABLE_DEBUG_OUTPUT
      if (data.user_percent > 0 || data.kernel_percent > 0) {
        DEBUG_PRINT("thread_id %u: user_percent=%u, kernel_percent=%u, "
                    "status=%u, flags=%u\n",
                    data.thread_id, data.user_percent, data.kernel_percent,
                    data.thread_status, data.extra_flags);
      }
#endif
    }

    // Write data to binary file
    if (!batch_data.empty()) {
      WriteDataToBinaryFile(batch_data);
    }
  }
  // Function to get total CPU time from /proc/stat
  double GetTotalCpuTime() const {
    std::ifstream file("/proc/stat");
    if (!file.is_open()) {
      fprintf(stderr, "Failed to open /proc/stat\n");
      return 0.0;
    }

    std::string line;
    if (!std::getline(file, line)) {
      fprintf(stderr, "Failed to read line from /proc/stat\n");
      return 0.0;
    }

    std::istringstream iss(line);
    std::string temp;
    uint64_t user, nice, system, idle, iowait, irq, softirq;
    if (!(iss >> temp >> user >> nice >> system >> idle >> iowait >> irq >>
          softirq)) {
      fprintf(stderr, "Error parsing /proc/stat\n");
      return 0.0;
    }

    DEBUG_PRINT(
        "Total CPU time ticks: user=%lu, nice=%lu, system=%lu, idle=%lu, "
        "iowait=%lu, irq=%lu, softirq=%lu\n",
        user, nice, system, idle, iowait, irq, softirq);

    const uint64_t total_time_ticks =
        user + nice + system + idle + iowait + irq + softirq;
    const double total_time_seconds =
        static_cast<double>(total_time_ticks) / ticks_per_second_;

    return total_time_seconds;
  }
  // Function to store current ticks as previous ticks
  void StoreCurrentTicksAsPrevious() {
    std::lock_guard<std::mutex> lck(data_mutex_);
    previous_ticks_ = current_ticks_;
    previous_total_cpu_time_ = current_total_cpu_time_;
  }

  // Function to parse /proc/[pid]/task/[tid]/stat file
  std::vector<uint64_t> ParseStatFile(const std::string &filename) const {
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

    // The second field can contain spaces enclosed in parentheses
    const size_t first_paren = line.find('(');
    const size_t last_paren = line.rfind(')');
    if (first_paren == std::string::npos || last_paren == std::string::npos ||
        last_paren <= first_paren) {
      fprintf(stderr, "Malformed stat file: %s\n", filename.c_str());
      return {};
    }

    std::string before_paren = line.substr(0, first_paren);
    std::string between_paren =
        line.substr(first_paren + 1, last_paren - first_paren - 1);
    std::string after_paren = line.substr(last_paren + 1);

    std::istringstream iss(after_paren);
    std::vector<std::string> fields;
    std::string field;

    // The first two fields are already processed
    fields.push_back(before_paren);
    fields.push_back(between_paren);

    while (iss >> field) {
      fields.push_back(field);
    }

    if (fields.size() <= KERNEL_TICKS_FIELD_INDEX) {
      fprintf(stderr, "Not enough fields in stat file: %s\n", filename.c_str());
      return {};
    }

    uint64_t user_time = std::stoull(fields[USER_TICKS_FIELD_INDEX]);
    uint64_t kernel_time = std::stoull(fields[KERNEL_TICKS_FIELD_INDEX]);

    return {user_time, kernel_time};
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
      // Get thread_id and name
      const std::string &thread_id_str = entry.first;
      const std::string &thread_name = entry.second;

      // Retrieve thread priority
      auto priority_it = thread_priorities_.find(thread_id_str);
      if (priority_it != thread_priorities_.end()) {
        int thread_priority = priority_it->second;
        fprintf(stdout, "  thread_name: %s, thread_id: %s,  Priority: %d\n",
                thread_name.c_str(), thread_id_str.c_str(), thread_priority);
      } else {
        fprintf(stdout,
                "  thread_name: %s, thread_id: %s,  Priority: Unknown\n",
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
        std::string comm_path = "/proc/" + pid_str + "/comm";
        std::ifstream comm_file(comm_path);
        if (comm_file.is_open()) {
          std::string comm_line;
          std::getline(comm_file, comm_line);
          if (comm_line == process_name) {
            pid = std::stoi(pid_str);
            return true;
          }
        }
      }
    }
  }

  return false;
}

// Function to parse command line arguments
bool ParseCommandLineArgs(int argc, char *argv[], int &pid,
                          double &refresh_delay, double &total_duration,
                          std::string &output_filename) {
  int opt;
  output_filename = "cpu_usage.bin"; // Default output filename
  std::string pid_or_name;

  // Update: Add new options -p, -d, -n
  while ((opt = getopt(argc, argv, "hp:o:n:d:")) != -1) {
    switch (opt) {
    case 'h':
      fprintf(stdout,
              "Usage: %s -p <Process_Name_or_PID> [-o <output_file>] "
              "[-n <total_seconds>] [-d <delay_seconds>] [-h]\n",
              argv[0]);
      fprintf(stdout, "Options:\n");
      fprintf(stdout,
              " -p <Process_Name_or_PID> Specify process name or PID.\n");
      fprintf(stdout, " -o <output_file> Set the output binary filename.\n");
      fprintf(stdout, " -n <total_seconds> Set the total monitoring duration "
                      "in seconds.\n");
      fprintf(stdout, " -d <delay_seconds> Set the CPU usage refresh interval "
                      "in seconds.\n");
      fprintf(stdout, " -h Display help.\n");
      return false;
    case 'p':
      pid_or_name = optarg;
      break;
    case 'o':
      output_filename = optarg;
      break;
    case 'n':
      try {
        total_duration = std::stod(optarg);
        if (total_duration <= 0.0) {
          fprintf(stderr, "Total duration must be a positive number.\n");
          return false;
        }
      } catch (const std::invalid_argument &) {
        fprintf(stderr, "Invalid total duration value: %s\n", optarg);
        return false;
      }
      break;
    case 'd':
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
    default:
      fprintf(stderr,
              "Usage: %s -p <Process_Name_or_PID> [-o <output_file>] "
              "[-n <total_seconds>] [-d <delay_seconds>] [-h]\n",
              argv[0]);
      return false;
    }
  }

  if (pid_or_name.empty()) {
    fprintf(stderr, "Missing process name or PID. Use -p option.\n");
    return false;
  }

  if (pid_or_name.find_first_not_of("0123456789") == std::string::npos) {
    pid = std::stoi(pid_or_name);
    std::string task_path = "/proc/" + std::to_string(pid) + "/task";
    DIR *dir = opendir(task_path.c_str());
    if (!dir) {
      fprintf(stderr, "Failed to find process with pid %d.\n", pid);
      return false;
    }
  } else {
    if (!FindPidByProcessName(pid_or_name, pid)) {
      fprintf(stderr, "Failed to find process with name: %s\n",
              pid_or_name.c_str());
      return false;
    }
  }
  fprintf(stdout, "Process PID or Name          : %s\n", pid_or_name.c_str());
  fprintf(stdout, "Sampling Delay (sec)         : %.2f\n", refresh_delay);
  fprintf(stdout, "Total Monitoring Time (sec)  : %.2f\n", total_duration);
  fprintf(stdout, "Output Filename              : %s\n",
          output_filename.c_str());
  return true;
}

} // namespace cpu_monitor

int main(int argc, char *argv[]) {
  setvbuf(stdout, NULL, _IONBF, 0);
  std::signal(SIGINT, cpu_monitor::SignalHandler);

  int pid = -1;
  double refresh_delay = 2.0;  // Default refresh delay in seconds
  double total_duration = 0.0; // Total monitoring duration in seconds
  std::string output_filename = "cpu_usage.bin"; // Default output filename

  if (!cpu_monitor::ParseCommandLineArgs(argc, argv, pid, refresh_delay,
                                         total_duration, output_filename)) {
    return 1;
  }

  try {
    cpu_monitor::CpuUsageMonitor monitor(pid, refresh_delay, total_duration,
                                         output_filename);
    monitor.Run();
  } catch (const std::exception &e) {
    fprintf(stderr, "Exception caught in monitor: %s\n", e.what());
    return 1;
  }

  return 0;
}
