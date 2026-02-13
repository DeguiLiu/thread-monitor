// Copyright 2025 Thread Monitor Authors
// SPDX-License-Identifier: MIT
//
// dummy_worker.cc - Configurable CPU load generator for testing cpu_monitor.
//
// Creates N worker threads, each occupying a target percentage of one CPU core
// via busy-wait + sleep cycles. Supports graceful shutdown via SIGINT/SIGTERM.

#include <pthread.h>
#include <sched.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Signal handling
// ---------------------------------------------------------------------------
static std::atomic<bool> g_running{true};

static void SignalHandler(int sig) {
  if (sig == SIGINT || sig == SIGTERM) {
    g_running.store(false, std::memory_order_relaxed);
  }
}

// ---------------------------------------------------------------------------
// Worker logic
// ---------------------------------------------------------------------------
static void BusyWait(int64_t ns) {
  auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - t0 <
         std::chrono::nanoseconds(ns)) {
    // Lightweight syscall to prevent aggressive optimization
    getpid();
  }
}

static void WorkerThread(int id, float rate) {
  // Set thread name (max 15 chars + null)
  char name[16];
  snprintf(name, sizeof(name), "worker_%d", id);
  pthread_setname_np(pthread_self(), name);

  // Set CPU affinity
  int ncpus = static_cast<int>(std::thread::hardware_concurrency());
  if (ncpus > 0) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(id % ncpus, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
  }

  // Try to set SCHED_FIFO (may fail without CAP_SYS_NICE)
  struct sched_param param;
  param.sched_priority = sched_get_priority_min(SCHED_FIFO) + id;
  if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
    // Non-fatal: fall back to default scheduler
  }

  constexpr int64_t kPeriodNs = 1000000;  // 1ms cycle
  auto busy_ns = static_cast<int64_t>(kPeriodNs * rate);
  auto sleep_ns = kPeriodNs - busy_ns;

  fprintf(stdout, "  [%s] tid=%d, cpu=%d/%d, rate=%.0f%%\n",
          name, static_cast<int>(gettid()), id % ncpus, ncpus, rate * 100);

  while (g_running.load(std::memory_order_relaxed)) {
    BusyWait(busy_ns);
    if (sleep_ns > 0) {
      std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));
    }
  }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
  if (argc < 3) {
    fprintf(stdout,
            "Usage: %s <num_workers> <occupation_rate>\n"
            "  num_workers:     number of worker threads (>= 1)\n"
            "  occupation_rate: CPU usage per thread, 0.0 ~ 1.0\n",
            argv[0]);
    return 0;
  }

  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);

  int num = atoi(argv[1]);
  float rate = static_cast<float>(atof(argv[2]));

  if (num < 1) {
    fprintf(stderr, "Error: num_workers must be >= 1\n");
    return 1;
  }
  if (rate < 0.0f || rate > 1.0f) {
    fprintf(stderr, "Error: occupation_rate must be in [0.0, 1.0]\n");
    return 1;
  }

  fprintf(stdout, "PID: %d, workers: %d, rate: %.0f%%\n",
          static_cast<int>(getpid()), num, rate * 100);

  std::vector<std::thread> threads;
  threads.reserve(static_cast<size_t>(num));
  for (int i = 0; i < num; ++i) {
    threads.emplace_back(WorkerThread, i, rate);
  }

  for (auto& t : threads) {
    if (t.joinable()) t.join();
  }

  fprintf(stdout, "\nAll workers stopped.\n");
  return 0;
}
