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

//  Define macro to control the output of debug information, enabled by default
#ifndef ENABLE_DEBUG_OUTPUT
#define ENABLE_DEBUG_OUTPUT 1
#endif

#if ENABLE_DEBUG_OUTPUT
#define DEBUG_PRINT(fmt, ...) fprintf(stdout, fmt, __VA_ARGS__)
#else
#define DEBUG_PRINT(fmt, ...)
#endif

#if defined(__aarch64__) || defined(RK3566)
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

namespace cpu_monitor {

// Atomic flag to handle graceful exit on SIGINT
std::atomic<bool> keep_running(true);

void SignalHandler(int signal) {
  if (signal == SIGINT) {
    keep_running = false;
  }
}

class CpuUsageMonitor {
 public:
  explicit CpuUsageMonitor(int pid, int refresh_delay, const std::string& log_filename)
      : pid_(pid),
        refresh_delay_(refresh_delay),
        previous_total_cpu_time_(0),
        current_total_cpu_time_(0),
        delta_total_cpu_time_(0),
        file_count_(1),
        buffer_size_(4096) {
    FILE* file = fopen(log_filename.c_str(), "w");
    if (!file) {
      fprintf(stderr, "Failed to open log file for writing: %s\n", log_filename.c_str());
      return;
    }
    log_file_ = std::make_unique<FileCloser>(file);
    buffer_ = std::make_unique<char[]>(buffer_size_);
    setvbuf(log_file_->get(), buffer_.get(), _IOFBF, buffer_size_);

    InitializeThreads();  // Initialize threads
    PrintProcessInfo();   // Print process and threads info
  }

  void Run() {
    GetThreadCpuTicks();
    current_total_cpu_time_ = GetTotalCpuTime();
    StoreCurrentTicksAsPrevious();

    while (keep_running) {
      std::this_thread::sleep_for(std::chrono::seconds(refresh_delay_));
      InitializeThreads();
      GetThreadCpuTicks();
      current_total_cpu_time_ = GetTotalCpuTime();
      delta_total_cpu_time_ = current_total_cpu_time_ - previous_total_cpu_time_;

      if (delta_total_cpu_time_ > 0) {
        ComputeCpuUsage();
        RotateLogFileIfNeeded();
      }

      StoreCurrentTicksAsPrevious();
    }

    fprintf(stdout, "Exiting gracefully...\n");
  }

 private:
  int pid_;
  int refresh_delay_;
  std::vector<std::string> threads_;
  std::map<std::string, std::pair<int64_t, int64_t>>
      previous_ticks_;  // key: thread ID, value: <user ticks, kernel ticks>
  std::map<std::string, std::pair<int64_t, int64_t>> current_ticks_;
  std::map<std::string, std::string> thread_names_;  // key: thread ID, value: thread name
  int64_t previous_total_cpu_time_;
  int64_t current_total_cpu_time_;
  int64_t delta_total_cpu_time_;
  std::unique_ptr<FileCloser> log_file_;  // Using FileCloser for RAII management
  std::mutex log_mutex_;
  int file_count_;
  size_t buffer_size_;
  std::unique_ptr<char[]> buffer_;

  // Initialize thread information from /proc filesystem
  void InitializeThreads() {
    std::lock_guard<std::mutex> lck(log_mutex_);
    threads_.clear();
    thread_names_.clear();
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
      if (std::isdigit(tid_str[0])) {
        threads_.push_back(tid_str);
        std::string comm_filename = task_path + "/" + tid_str + "/comm";
        std::ifstream comm_file(comm_filename);
        if (comm_file.is_open()) {
          std::string thread_name;
          std::getline(comm_file, thread_name);
          thread_names_[tid_str] = thread_name;
          DEBUG_PRINT("Thread ID: %s, Name: %s\n", tid_str.c_str(), thread_name.c_str());
        } else {
          fprintf(stderr, "Failed to open file: %s\n", comm_filename.c_str());
        }
      }
    }
  }

  // Parse the /proc/[pid]/task/[tid]/stat file to get user and kernel ticks
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

    // Skip fields up to user and kernel ticks
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

  // Get CPU ticks for all threads of the process
  void GetThreadCpuTicks() {
    std::lock_guard<std::mutex> lck(log_mutex_);
    current_ticks_.clear();
    for (const auto& thread : threads_) {
      std::string stat_filename = "/proc/" + std::to_string(pid_) + "/task/" + thread + "/stat";
      auto thread_data = ParseStatFile(stat_filename);
      if (thread_data.size() == 2) {
        current_ticks_[thread] = {thread_data[0], thread_data[1]};
      }
    }
  }

  // Get the total CPU time from /proc/stat
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

    return user + nice + system + idle + iowait + irq + softirq;
  }

  // Compute CPU usage for each thread and log it
  void ComputeCpuUsage() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::

        tm buf;
    localtime_r(&in_time_t, &buf);

    std::lock_guard<std::mutex> log_lock(log_mutex_);
    for (const auto& thread : threads_) {
      int64_t user_delta = 0;
      int64_t kernel_delta = 0;

      if (previous_ticks_.find(thread) != previous_ticks_.end()) {
        user_delta = current_ticks_.at(thread).first - previous_ticks_.at(thread).first;
        kernel_delta = current_ticks_.at(thread).second - previous_ticks_.at(thread).second;
      }

      double user_percent = 0.0;
      double kernel_percent = 0.0;

      if (delta_total_cpu_time_ > 0) {
        user_percent = static_cast<double>(user_delta) / delta_total_cpu_time_ * 100.0;
        kernel_percent = static_cast<double>(kernel_delta) / delta_total_cpu_time_ * 100.0;
      }

      fprintf(log_file_->get(), "%02d:%02d:%02d,%s,%s,%d,%d\n", buf.tm_hour, buf.tm_min, buf.tm_sec,
              thread_names_.at(thread).c_str(), thread.c_str(), static_cast<int>(user_percent),
              static_cast<int>(kernel_percent));
      fflush(log_file_->get());  // Ensure immediate writing to the log file
    }
  }

  // Store current ticks as previous ticks for the next iteration
  void StoreCurrentTicksAsPrevious() {
    std::lock_guard<std::mutex> lck(log_mutex_);
    previous_ticks_ = current_ticks_;
    previous_total_cpu_time_ = current_total_cpu_time_;
  }

  // Print process and thread information at the start of the log
  void PrintProcessInfo() {
    int process_priority = getpriority(PRIO_PROCESS, pid_);
    if (process_priority == -1 && errno != 0) {
      fprintf(stderr, "Failed to get process priority: %s\n", strerror(errno));
    } else {
      fprintf(stdout, "Process ID: %d\n", pid_);
      fprintf(stdout, "Process Priority: %d\n", process_priority);
      fprintf(log_file_->get(), "Process ID: %d\n", pid_);
      fprintf(log_file_->get(), "Process Priority: %d\n", process_priority);
    }

    for (const auto& thread : threads_) {
      std::string stat_filename = "/proc/" + std::to_string(pid_) + "/task/" + thread + "/stat";
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

      // Skip to the appropriate fields based on architecture
      for (int i = 0; i < PRIORITY_FIELD_INDEX; ++i) {
        if (!(iss >> temp)) {
          fprintf(stderr, "Error parsing stat file: %s\n", stat_filename.c_str());
          continue;
        }
      }

      // Get the priority and nice value
      if (!(iss >> priority >> nice_value)) {
        fprintf(stderr, "Error parsing priority/nice value from stat file: %s\n", stat_filename.c_str());
        continue;
      }

      DEBUG_PRINT("Thread ID: %s, Name: %s, Priority: %d, Nice: %d\n", thread.c_str(), thread_names_.at(thread).c_str(),
                  priority, nice_value);
      fprintf(log_file_->get(), "Thread ID: %s, Name: %s, Priority: %d, Nice: %d\n", thread.c_str(),
              thread_names_.at(thread).c_str(), priority, nice_value);
    }
    fputs("Time,Thread Name,Thread ID,User %,Kernel %\n", log_file_->get());  // CSV Header
    fflush(log_file_->get());
  }

  // Rotate log file if it exceeds a certain size (e.g., 10 MB)
  void RotateLogFileIfNeeded() {
    const std::streampos max_size = 10 * 1024 * 1024;  // 10 MB
    if (ftell(log_file_->get()) >= max_size) {
      fflush(log_file_->get());
      fclose(log_file_->get());

      std::string new_filename =
          "cpu_usage_log_" + std::to_string(pid_) + "_part_" + std::to_string(file_count_++) + ".csv";
      FILE* file = fopen(new_filename.c_str(), "w");
      if (!file) {
        fprintf(stderr, "Failed to open new log file for writing: %s\n", new_filename.c_str());
        return;
      }
      log_file_ = std::make_unique<FileCloser>(file);
      setvbuf(log_file_->get(), buffer_.get(), _IOFBF, buffer_size_);
      PrintProcessInfo();
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

bool ParseCommandLineArgs(int argc, char* argv[], std::vector<std::pair<int, std::string>>& pid_log_pairs,
                          int& refresh_delay, std::string& base_log_filename) {
  int opt;
  while ((opt = getopt(argc, argv, "hn:o:")) != -1) {
    switch (opt) {
      case 'h':
        fprintf(stdout, "Usage: %s [options] <pid_or_process_name>...\n", argv[0]);
        fprintf(stdout, "Options:\n");
        fprintf(stdout, " -h Display help\n");
        fprintf(stdout, " -n <delay> Set the display refresh value in sec.\n");
        fprintf(stdout, " -o <filename> Set the base output CSV log file name.\n");
        return false;
      case 'n':
        refresh_delay = std::stoi(optarg);
        if (refresh_delay <= 0) {
          fprintf(stderr, "Refresh delay must be a positive integer.\n");
          return false;
        }
        break;
      case 'o':
        base_log_filename = optarg;
        break;
      default:
        fprintf(stderr, "Usage: %s [options] <pid_or_process_name>...\n", argv[0]);
        return false;
    }
  }

  for (int i = optind; i < argc; ++i) {
    std::string pid_or_name = argv[i];
    int pid = -1;
    std::string log_filename;

    if (pid_or_name.find_first_not_of("0123456789") == std::string::npos) {
      pid = std::stoi(pid_or_name);
      log_filename = base_log_filename.empty() ? "process_" + std::to_string(pid) + ".csv"
                                               : base_log_filename + "_pid_" + std::to_string(pid) + ".csv";
    } else {
      if (!FindPidByProcessName(pid_or_name, pid)) {
        fprintf(stderr, "Failed to find process with name: %s\n", pid_or_name.c_str());
        return false;
      }
      log_filename = base_log_filename.empty() ? "process_" + pid_or_name + ".csv"
                                               : base_log_filename + "_" + pid_or_name + ".csv";
    }

    pid_log_pairs.emplace_back(pid, log_filename);
  }

  if (pid_log_pairs.empty()) {
    fprintf(stderr, "Missing PID or process name.\n");
    return false;
  }

  return true;
}

int main(int argc, char* argv[]) {
  std::signal(SIGINT, cpu_monitor::SignalHandler);

  std::vector<std::pair<int, std::string>> pid_log_pairs;
  int refresh_delay = 2;
  std::string base_log_filename;

  if (!ParseCommandLineArgs(argc, argv, pid_log_pairs, refresh_delay, base_log_filename)) {
    return 1;
  }

  std::vector<std::thread> monitor_threads;
  for (const auto& pid_log : pid_log_pairs) {
    monitor_threads.emplace_back([pid_log, refresh_delay]() {
      try {
        cpu_monitor::CpuUsageMonitor monitor(pid_log.first, refresh_delay, pid_log.second);
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
