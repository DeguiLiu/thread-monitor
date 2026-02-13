// Copyright 2025 Thread Monitor Authors
// SPDX-License-Identifier: MIT
//
// cpu_monitor.cc - Per-thread CPU usage monitor for Linux processes.
//
// Reads /proc filesystem to sample per-thread CPU ticks at fixed intervals
// using steady_clock, computes usage percentages, writes binary records,
// and optionally displays real-time terminal output.

#include "proc_parser.hpp"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Binary record layout (packed, 16 bytes)
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct CpuUsageRecord {
  uint32_t timestamp;       // Seconds since epoch
  uint32_t thread_id;       // TID
  uint16_t user_percent;    // 0-10000 (100.00%)
  uint16_t kernel_percent;  // 0-10000 (100.00%)
  uint8_t state;            // Thread state char
  uint8_t processor;        // Last CPU core
  int8_t priority;          // Scheduling priority
  int8_t nice;              // Nice value
};
#pragma pack(pop)

static_assert(sizeof(CpuUsageRecord) == 16, "CpuUsageRecord must be 16 bytes");

// ---------------------------------------------------------------------------
// File format constants
// ---------------------------------------------------------------------------
static constexpr uint32_t kMagic = 0x4E4F4D43;       // "CMON" LE
static constexpr uint32_t kVersion = 2;
static constexpr uint32_t kFooterMagic = 0x444E4543;  // "CEND" LE
static constexpr uint32_t kThreadRescanInterval = 5;

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
// Thread snapshot (pre-allocated, reused across iterations)
// ---------------------------------------------------------------------------
struct ThreadSnapshot {
  uint32_t tid;
  char name[32];
  uint64_t utime;
  uint64_t stime;
  char state;
  int32_t priority;
  int32_t nice;
  int32_t processor;
};

// ---------------------------------------------------------------------------
// Tick cache entry (flat vector replaces std::map)
// ---------------------------------------------------------------------------
struct TickEntry {
  uint32_t tid;
  uint64_t utime;
  uint64_t stime;
};

// ---------------------------------------------------------------------------
// BinaryWriter - file I/O for the binary format
// ---------------------------------------------------------------------------
class BinaryWriter {
 public:
  BinaryWriter() = default;
  ~BinaryWriter() { Close(); }

  BinaryWriter(const BinaryWriter&) = delete;
  BinaryWriter& operator=(const BinaryWriter&) = delete;

  bool Open(const char* filename) {
    fp_ = fopen(filename, "wb");
    return fp_ != nullptr;
  }

  void WriteHeader(const char* process_name, uint32_t num_cpus,
                   uint32_t ticks_per_sec,
                   const std::vector<ThreadSnapshot>& threads) {
    if (!fp_) return;
    WriteU32(kMagic);
    WriteU32(kVersion);

    long size_pos = ftell(fp_);
    WriteU32(0);
    long start = ftell(fp_);

    auto plen = static_cast<uint32_t>(strlen(process_name));
    WriteU32(plen);
    fwrite(process_name, 1, plen, fp_);
    WriteU32(num_cpus);
    WriteU32(ticks_per_sec);
    WriteThreadMap(threads);

    long end = ftell(fp_);
    auto hsize = static_cast<uint32_t>(end - start);
    fseek(fp_, size_pos, SEEK_SET);
    WriteU32(hsize);
    fseek(fp_, end, SEEK_SET);
    fflush(fp_);
  }

  void WriteRecords(const CpuUsageRecord* records, uint32_t count) {
    if (!fp_ || count == 0) return;
    fwrite(records, sizeof(CpuUsageRecord), count, fp_);
    fflush(fp_);
  }

  void WriteFooter(const std::vector<ThreadSnapshot>& new_threads) {
    if (!fp_) return;
    auto offset = static_cast<uint32_t>(ftell(fp_));
    WriteU32(kFooterMagic);
    WriteThreadMap(new_threads);
    WriteU32(offset);
    fflush(fp_);
  }

  void Close() {
    if (fp_) {
      fclose(fp_);
      fp_ = nullptr;
    }
  }

 private:
  FILE* fp_ = nullptr;

  void WriteU32(uint32_t val) { fwrite(&val, sizeof(val), 1, fp_); }

  void WriteThreadMap(const std::vector<ThreadSnapshot>& threads) {
    auto count = static_cast<uint32_t>(threads.size());
    WriteU32(count);
    for (const auto& t : threads) {
      WriteU32(t.tid);
      auto nlen = static_cast<uint32_t>(strlen(t.name));
      WriteU32(nlen);
      fwrite(t.name, 1, nlen, fp_);
    }
  }
};

// ---------------------------------------------------------------------------
// CpuMonitor - main monitoring class
// ---------------------------------------------------------------------------
class CpuMonitor {
 public:
  CpuMonitor(int pid, double interval_sec, double duration_sec,
             const char* output, bool realtime)
      : pid_(pid),
        interval_ns_(static_cast<int64_t>(interval_sec * 1e9)),
        duration_ns_(static_cast<int64_t>(duration_sec * 1e9)),
        interval_sec_(interval_sec),
        output_file_(output),
        realtime_(realtime),
        ticks_per_sec_(static_cast<uint32_t>(sysconf(_SC_CLK_TCK))),
        num_cpus_(static_cast<uint32_t>(sysconf(_SC_NPROCESSORS_ONLN))) {}

  int Run() {
    char pname[64];
    if (!proc::IsOk(proc::ReadProcessName(pid_, pname, sizeof(pname)))) {
      fprintf(stderr, "Error: cannot read process name for PID %d\n", pid_);
      return 1;
    }

    ScanThreads();
    if (threads_.empty()) {
      fprintf(stderr, "Error: no threads found for PID %d\n", pid_);
      return 1;
    }

    if (!writer_.Open(output_file_)) {
      fprintf(stderr, "Error: cannot open %s for writing\n", output_file_);
      return 1;
    }
    writer_.WriteHeader(pname, num_cpus_, ticks_per_sec_, threads_);

    // Pre-allocate record buffer
    records_.reserve(threads_.size());

    fprintf(stdout, "Monitoring: %s (PID %d), %u threads, %u CPUs\n",
            pname, pid_, static_cast<unsigned>(threads_.size()), num_cpus_);
    fprintf(stdout, "Interval: %.2fs, Duration: %s, Output: %s\n",
            interval_sec_,
            duration_ns_ > 0
                ? (std::to_string(duration_sec_()) + "s").c_str()
                : "unlimited",
            output_file_);

    // Initial sample
    proc::ReadSystemCpuTicks(&prev_sys_);
    SampleTicks(prev_ticks_);

    using Clock = std::chrono::steady_clock;
    auto start = Clock::now();
    auto next_sample = start + std::chrono::nanoseconds(interval_ns_);
    uint32_t iteration = 0;

    while (g_running.load(std::memory_order_relaxed)) {
      // Fixed-period sleep using steady_clock
      SleepUntil(next_sample);
      next_sample += std::chrono::nanoseconds(interval_ns_);

      if (duration_ns_ > 0) {
        auto elapsed = Clock::now() - start;
        if (elapsed >= std::chrono::nanoseconds(duration_ns_)) break;
      }

      if (!proc::IsProcessAlive(pid_)) {
        fprintf(stdout, "\nProcess %d exited.\n", pid_);
        break;
      }

      if (iteration % kThreadRescanInterval == 0) {
        ScanThreads();
        records_.reserve(threads_.size());
      }

      proc::SystemCpuTicks cur_sys{};
      proc::ReadSystemCpuTicks(&cur_sys);
      SampleTicks(cur_ticks_);

      uint64_t delta_total = cur_sys.Total() - prev_sys_.Total();
      if (delta_total == 0) {
        prev_sys_ = cur_sys;
        std::swap(prev_ticks_, cur_ticks_);
        ++iteration;
        continue;
      }

      auto now_epoch = static_cast<uint32_t>(
          std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count());

      records_.clear();
      for (const auto& t : threads_) {
        uint64_t du = 0, ds = 0;
        FindTickDelta(t.tid, &du, &ds);

        auto user_pct = static_cast<uint16_t>(
            std::min(du * num_cpus_ * 10000ULL / delta_total, 10000ULL));
        auto kern_pct = static_cast<uint16_t>(
            std::min(ds * num_cpus_ * 10000ULL / delta_total, 10000ULL));

        CpuUsageRecord rec{};
        rec.timestamp = now_epoch;
        rec.thread_id = t.tid;
        rec.user_percent = user_pct;
        rec.kernel_percent = kern_pct;
        rec.state = static_cast<uint8_t>(t.state);
        rec.processor = static_cast<uint8_t>(t.processor);
        rec.priority = static_cast<int8_t>(t.priority);
        rec.nice = static_cast<int8_t>(t.nice);
        records_.push_back(rec);
      }

      writer_.WriteRecords(records_.data(),
                           static_cast<uint32_t>(records_.size()));

      if (realtime_) {
        PrintRealtime(pname);
      } else {
        fprintf(stdout, ".");
        fflush(stdout);
      }

      prev_sys_ = cur_sys;
      std::swap(prev_ticks_, cur_ticks_);
      ++iteration;
    }

    writer_.WriteFooter(new_threads_);
    writer_.Close();
    fprintf(stdout, "\nDone. %s written.\n", output_file_);
    return 0;
  }

 private:
  int pid_;
  int64_t interval_ns_;
  int64_t duration_ns_;
  double interval_sec_;
  const char* output_file_;
  bool realtime_;
  uint32_t ticks_per_sec_;
  uint32_t num_cpus_;

  // Pre-allocated containers (reused across iterations)
  std::vector<ThreadSnapshot> threads_;
  std::vector<ThreadSnapshot> new_threads_;
  std::vector<uint32_t> known_tids_;
  std::vector<CpuUsageRecord> records_;
  std::vector<TickEntry> prev_ticks_;
  std::vector<TickEntry> cur_ticks_;
  std::vector<size_t> sort_idx_;
  proc::SystemCpuTicks prev_sys_{};
  BinaryWriter writer_;
  char path_buf_[96];  // Reused path buffer

  double duration_sec_() const {
    return static_cast<double>(duration_ns_) / 1e9;
  }

  void ScanThreads() {
    threads_.clear();
    proc::EnumerateThreads(pid_, [this](int tid) {
      ThreadSnapshot snap{};
      snap.tid = static_cast<uint32_t>(tid);

      snprintf(path_buf_, sizeof(path_buf_),
               "/proc/%d/task/%d/comm", pid_, tid);
      proc::ReadThreadName(path_buf_, snap.name, sizeof(snap.name));

      snprintf(path_buf_, sizeof(path_buf_),
               "/proc/%d/task/%d/stat", pid_, tid);
      proc::ThreadStat ts{};
      if (proc::IsOk(proc::ReadThreadStat(path_buf_, &ts))) {
        snap.utime = ts.utime;
        snap.stime = ts.stime;
        snap.state = ts.state;
        snap.priority = ts.priority;
        snap.nice = ts.nice;
        snap.processor = ts.processor;
      }

      threads_.push_back(snap);

      if (!IsKnownTid(snap.tid)) {
        if (!known_tids_.empty()) {
          new_threads_.push_back(snap);
        }
        known_tids_.push_back(snap.tid);
      }
    });
  }

  bool IsKnownTid(uint32_t tid) const {
    for (uint32_t k : known_tids_) {
      if (k == tid) return true;
    }
    return false;
  }

  void SampleTicks(std::vector<TickEntry>& ticks) {
    ticks.clear();
    ticks.reserve(threads_.size());
    for (const auto& t : threads_) {
      snprintf(path_buf_, sizeof(path_buf_),
               "/proc/%d/task/%d/stat", pid_, static_cast<int>(t.tid));
      proc::ThreadStat ts{};
      if (proc::IsOk(proc::ReadThreadStat(path_buf_, &ts))) {
        ticks.push_back({t.tid, ts.utime, ts.stime});
      }
    }
  }

  void FindTickDelta(uint32_t tid, uint64_t* du, uint64_t* ds) const {
    const TickEntry* cur = nullptr;
    const TickEntry* prev = nullptr;
    for (const auto& e : cur_ticks_) {
      if (e.tid == tid) {
        cur = &e;
        break;
      }
    }
    for (const auto& e : prev_ticks_) {
      if (e.tid == tid) {
        prev = &e;
        break;
      }
    }
    if (cur && prev) {
      *du = cur->utime - prev->utime;
      *ds = cur->stime - prev->stime;
    } else {
      *du = 0;
      *ds = 0;
    }
  }

  void PrintRealtime(const char* pname) {
    fprintf(stdout, "\033[2J\033[H");
    fprintf(stdout, "Process: %s (PID %d)  CPUs: %u  Interval: %.1fs\n\n",
            pname, pid_, num_cpus_, interval_sec_);
    fprintf(stdout, "%-8s %-20s %6s %6s %6s %4s %4s %3s\n",
            "TID", "Name", "User%", "Kern%", "Total", "CPU", "Pri", "St");
    fprintf(stdout,
            "-------- -------------------- ------ ------ ------ ---- ---- ---\n");

    sort_idx_.resize(records_.size());
    for (size_t i = 0; i < sort_idx_.size(); ++i) sort_idx_[i] = i;
    std::sort(sort_idx_.begin(), sort_idx_.end(),
              [this](size_t a, size_t b) {
      return (records_[a].user_percent + records_[a].kernel_percent) >
             (records_[b].user_percent + records_[b].kernel_percent);
    });

    for (size_t i : sort_idx_) {
      const auto& r = records_[i];
      double u = r.user_percent / 100.0;
      double k = r.kernel_percent / 100.0;
      const char* name = "?";
      for (const auto& t : threads_) {
        if (t.tid == r.thread_id) {
          name = t.name;
          break;
        }
      }
      fprintf(stdout, "%-8u %-20.20s %6.1f %6.1f %6.1f %4u %4d  %c\n",
              r.thread_id, name, u, k, u + k,
              static_cast<unsigned>(r.processor),
              static_cast<int>(r.priority),
              static_cast<char>(r.state));
    }
    fprintf(stdout, "\nPress Ctrl+C to stop.\n");
    fflush(stdout);
  }

  static void SleepUntil(std::chrono::steady_clock::time_point target) {
    auto now = std::chrono::steady_clock::now();
    if (now >= target) return;
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        target - now);
    struct timespec ts;
    ts.tv_sec = static_cast<time_t>(ns.count() / 1000000000LL);
    ts.tv_nsec = static_cast<long>(ns.count() % 1000000000LL);
    nanosleep(&ts, nullptr);
  }
};

// ---------------------------------------------------------------------------
// Command-line parsing
// ---------------------------------------------------------------------------
struct Options {
  int pid = -1;
  double interval = 1.0;
  double duration = 0.0;
  const char* output = "cpu_usage.bin";
  bool realtime = false;
};

static void PrintUsage(const char* prog) {
  fprintf(stdout,
      "Usage: %s -p <PID|name> [options]\n\n"
      "Options:\n"
      "  -p <PID|name>   Process PID or name (required)\n"
      "  -d <seconds>    Sampling interval (default: 1.0)\n"
      "  -n <seconds>    Total monitoring duration (0=unlimited)\n"
      "  -o <file>       Output binary file (default: cpu_usage.bin)\n"
      "  -r              Enable real-time terminal display\n"
      "  -h              Show this help\n",
      prog);
}

static bool ParseArgs(int argc, char* argv[], Options* opts) {
  int opt;
  const char* pid_or_name = nullptr;

  while ((opt = getopt(argc, argv, "hp:o:n:d:r")) != -1) {
    switch (opt) {
      case 'h':
        PrintUsage(argv[0]);
        return false;
      case 'p':
        pid_or_name = optarg;
        break;
      case 'o':
        opts->output = optarg;
        break;
      case 'r':
        opts->realtime = true;
        break;
      case 'n':
        opts->duration = atof(optarg);
        if (opts->duration < 0) {
          fprintf(stderr, "Error: duration must be >= 0\n");
          return false;
        }
        break;
      case 'd':
        opts->interval = atof(optarg);
        if (opts->interval <= 0) {
          fprintf(stderr, "Error: interval must be > 0\n");
          return false;
        }
        break;
      default:
        PrintUsage(argv[0]);
        return false;
    }
  }

  if (!pid_or_name) {
    fprintf(stderr, "Error: -p <PID|name> is required.\n");
    PrintUsage(argv[0]);
    return false;
  }

  bool is_numeric = true;
  for (const char* p = pid_or_name; *p; ++p) {
    if (*p < '0' || *p > '9') {
      is_numeric = false;
      break;
    }
  }

  if (is_numeric) {
    opts->pid = atoi(pid_or_name);
  } else {
    opts->pid = proc::FindPidByName(pid_or_name);
    if (opts->pid < 0) {
      fprintf(stderr, "Error: process '%s' not found.\n", pid_or_name);
      return false;
    }
    fprintf(stdout, "Found process '%s' with PID %d\n",
            pid_or_name, opts->pid);
  }

  if (!proc::IsProcessAlive(opts->pid)) {
    fprintf(stderr, "Error: PID %d does not exist.\n", opts->pid);
    return false;
  }

  return true;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);

  Options opts;
  if (!ParseArgs(argc, argv, &opts)) {
    return 1;
  }

  CpuMonitor monitor(opts.pid, opts.interval, opts.duration,
                      opts.output, opts.realtime);
  return monitor.Run();
}
