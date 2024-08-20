#include <pthread.h>
#include <sched.h>
#include <unistd.h>  // For getpid() and other system calls
#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace worker_app {

void BusyWait(std::size_t nanosec) {
  const auto t0 = std::chrono::high_resolution_clock::now();

  while (std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - t0).count() <
         nanosec) {
    // Perform simple system calls during the busy-wait loop
    getpid();       // This call is simple but ensures a switch to kernel mode
    sched_yield();  // Yield the processor, another system call to engage the kernel
  }
}

[[noreturn]] void Work(float percentage, int thread_id) {
  assert(percentage >= 0.0f && percentage <= 1.0f);
  constexpr float kPeriod = 1'000'000.0f;

  // Set thread name
  const std::string thread_name = "worker_" + std::to_string(thread_id);
  (void)pthread_setname_np(pthread_self(), thread_name.c_str());

  // Set CPU affinity to ensure the thread uses a specific CPU core
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(static_cast<int>(thread_id % std::thread::hardware_concurrency()), &cpuset);
  (void)pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

  // Set thread scheduling policy and priority
  struct sched_param param;
  param.sched_priority = sched_get_priority_min(SCHED_FIFO) + thread_id;  // Vary priority by thread_id
  if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
    std::cerr << "Failed to set thread scheduling policy and priority for thread " << thread_id << "\n";
  }

  while (true) {
    BusyWait(static_cast<std::size_t>(kPeriod * percentage));
    std::this_thread::sleep_for(std::chrono::nanoseconds(static_cast<std::size_t>(kPeriod * (1.0f - percentage))));
  }
}

}  // namespace worker_app

int main(int argc, char* argv[]) {
  if (argc < 3) {
    std::cout << "Args: worker_num occupation_rate.\n";
    return 0;
  }
  const int num = std::stoi(argv[1]);
  const float percentage = std::stof(argv[2]);
  if (num < 1) {
    std::cout << "Error: num of workers less than 1.\n";
    return 0;
  }
  if (percentage < 0.0f || percentage > 1.0f) {
    std::cout << "Error: occupation rate should be between [0.0, 1.0].\n";
    return 0;
  }
  std::cout << "num of workers: " << num << "\n"
            << "occupation rate: " << percentage << "\n";

  // Create and start worker threads
  std::vector<std::unique_ptr<std::thread>> threads;
  threads.reserve(num);
  for (int i = 0; i < num; ++i) {
    threads.push_back(std::make_unique<std::thread>(worker_app::Work, percentage, i));
  }

  // Join all threads
  for (auto& td : threads) {
    if (td->joinable()) {
      td->join();
    }
  }

  return 0;
}
