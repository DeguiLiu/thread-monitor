#include <cassert>
#include <chrono>
#include <cstdio>  // For fprintf
#include <cstring> // For strerror
#include <pthread.h>
#include <sched.h>
#include <string>
#include <sys/prctl.h>   // For prctl()
#include <sys/syscall.h> // For SYS_gettid
#include <system_error>  // For std::system_error
#include <thread>
#include <unistd.h> // For getpid() and syscall()
#include <vector>

namespace worker_app {

// Get the current thread's system thread ID
pid_t GetThreadID() { return static_cast<pid_t>(syscall(SYS_gettid)); }

// Get the priority of the thread
int GetThreadPriority(pthread_t thread) {
  int policy;
  struct sched_param param;
  if (pthread_getschedparam(thread, &policy, &param) != 0) {
    return -1; // Failed to get priority
  }
  return param.sched_priority;
}

// Busy-wait function, adding system calls to avoid excessive compiler
// optimizations
void BusyWait(std::size_t nanosec) {
  const auto t0 = std::chrono::high_resolution_clock::now();

  while (std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::high_resolution_clock::now() - t0)
             .count() < nanosec) {
    // Simple system calls to ensure switching to kernel mode
    getpid();
    sched_yield();
  }
}

// Worker thread function
[[noreturn]] void Work(float percentage, int thread_id) {
  assert(percentage >= 0.0f && percentage <= 1.0f);
  constexpr float kPeriod = 1'000'000.0f;

  // Set thread name
  std::string thread_name = "worker_" + std::to_string(thread_id);
  if (pthread_setname_np(pthread_self(), thread_name.c_str()) != 0) {
    fprintf(stderr, "Failed to set thread name for thread %d: %s\n", thread_id,
            strerror(errno));
  }

  // Set CPU affinity to ensure the thread runs on a specific CPU core
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(thread_id % static_cast<int>(std::thread::hardware_concurrency()),
          &cpuset);
  if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0) {
    fprintf(stderr, "Failed to set CPU affinity for thread %d: %s\n", thread_id,
            strerror(errno));
  }

  // Set thread scheduling policy and priority
  struct sched_param param;
  int min_priority = sched_get_priority_min(SCHED_FIFO);
  if (min_priority == -1) {
    fprintf(stderr, "Failed to get minimum priority: %s\n", strerror(errno));
  }
  param.sched_priority = min_priority + thread_id;
  if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
    fprintf(stderr, "Failed to set thread scheduling for thread %d: %s\n",
            thread_id, strerror(errno));
  }

  // Print thread information with adjusted priority
  pid_t tid = GetThreadID();
  int sched_priority = GetThreadPriority(pthread_self());
  if (sched_priority == -1) {
    fprintf(stderr, "Failed to get thread priority for thread %d.\n",
            thread_id);
  }

  // Adjust priority to match 'top' display (PR = -(sched_priority + 1))
  int display_priority = -(sched_priority + 1);
  fprintf(stdout, "Thread Name: %s, Thread ID: %d, Priority: %d\n",
          thread_name.c_str(), tid, display_priority);

  // Main loop for the thread
  while (true) {
    BusyWait(static_cast<std::size_t>(kPeriod * percentage));
    std::this_thread::sleep_for(std::chrono::nanoseconds(
        static_cast<std::size_t>(kPeriod * (1.0f - percentage))));
  }
}

} // namespace worker_app

// Get process name
std::string GetProcessName() {
  char name[256] = {0};
  if (prctl(PR_GET_NAME, name, 0, 0, 0) != 0) {
    return "Unknown";
  }
  return std::string(name);
}

int main(int argc, char *argv[]) {
  if (argc < 3) {
    fprintf(stdout, "Usage: %s <worker_num> <occupation_rate>\n", argv[0]);
    return 0;
  }

  int num = 0;
  float percentage = 0.0f;

  try {
    num = std::stoi(argv[1]);
    percentage = std::stof(argv[2]);
  } catch (const std::invalid_argument &e) {
    fprintf(stderr,
            "Invalid arguments. Please provide valid integers and floats.\n");
    return 1;
  } catch (const std::out_of_range &e) {
    fprintf(stderr, "Arguments out of range.\n");
    return 1;
  }

  if (num < 1) {
    fprintf(stderr, "Error: Number of workers must be at least 1.\n");
    return 1;
  }

  if (percentage < 0.0f || percentage > 1.0f) {
    fprintf(stderr, "Error: Occupation rate should be between [0.0, 1.0].\n");
    return 1;
  }

  // Print process information
  std::string process_name = GetProcessName();
  pid_t pid = getpid();
  fprintf(stdout,
          "Process Name: %s\nProcess ID: %d\nNumber of Workers: %d\nOccupation "
          "Rate: %.2f\n",
          process_name.c_str(), pid, num, percentage);

  // Create and start worker threads
  std::vector<std::thread> threads;
  threads.reserve(num);
  for (int i = 0; i < num; ++i) {
    try {
      threads.emplace_back(worker_app::Work, percentage, i);
    } catch (const std::system_error &e) {
      fprintf(stderr, "Failed to create thread %d: %s\n", i, e.what());
    }
  }

  // Main thread waits for all worker threads (since worker threads run
  // indefinitely, the main thread will block here)
  for (auto &th : threads) {
    if (th.joinable()) {
      th.join();
    }
  }

  return 0;
}
