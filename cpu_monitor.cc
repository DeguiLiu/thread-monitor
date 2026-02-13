// Copyright 2025 Thread Monitor Authors
// SPDX-License-Identifier: MIT
//
// cpu_monitor.cc - Per-thread CPU usage monitor for Linux processes.
//
// Architecture:
//   proc_parser.hpp  -> text-to-struct parsing (no business logic)
//   cpu_monitor.cc   -> scheduling, sampling, statistics
//   OutputSink       -> pluggable output (binary file, console, etc.)
//
// Sampling uses steady_clock with "next target time" scheduling to avoid
// cumulative drift. Exit summary reports missed samples and jitter stats.

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
// Tick cache entry (flat vector, index-aligned with threads_)
// ---------------------------------------------------------------------------
struct TickEntry {
  uint32_t tid;
  uint64_t utime;
  uint64_t stime;
};

// ---------------------------------------------------------------------------
// Sampling statistics (jitter tracking)
// ---------------------------------------------------------------------------
struct SamplingStats {
  uint32_t total_samples = 0;
  uint32_t missed_samples = 0;   // Samples where actual > 2x interval
  int64_t sum_latency_ns = 0;    // Sum of |actual - target| for avg calc
  int64_t max_latency_ns = 0;    // Worst-case jitter

  void RecordSample(int64_t latency_ns) {
    ++total_samples;
    int64_t abs_lat = latency_ns < 0 ? -latency_ns : latency_ns;
    sum_latency_ns += abs_lat;
    if (abs_lat > max_latency_ns) max_latency_ns = abs_lat;
  }

  void RecordMiss() { ++missed_samples; }

  double AvgLatencyUs() const {
    if (total_samples == 0) return 0.0;
    return static_cast<double>(sum_latency_ns) /
           static_cast<double>(total_samples) / 1000.0;
  }

  double MaxLatencyUs() const {
    return static_cast<double>(max_latency_ns) / 1000.0;
  }
};

// ---------------------------------------------------------------------------
// OutputSink - abstract output interface
// ---------------------------------------------------------------------------
class OutputSink {
 public:
  virtual ~OutputSink() = default;
  virtual void OnHeader(const char* process_name, uint32_t num_cpus,
                        uint32_t ticks_per_sec,
                        const std::vector<ThreadSnapshot>& threads) = 0;
  virtual void OnRecords(const CpuUsageRecord* records, uint32_t count,
                         const std::vector<ThreadSnapshot>& threads) = 0;
  virtual void OnFooter(const std::vector<ThreadSnapshot>& new_threads) = 0;
  virtual void OnClose() = 0;
};

// ---------------------------------------------------------------------------
// BinaryFileSink - writes binary format v2
// ---------------------------------------------------------------------------
class BinaryFileSink : public OutputSink {
 public:
  bool Open(const char* filename) {
    fp_ = fopen(filename, "wb");
    return fp_ != nullptr;
  }

  void OnHeader(const char* process_name, uint32_t num_cpus,
                uint32_t ticks_per_sec,
                const std::vector<ThreadSnapshot>& threads) override {
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

  void OnRecords(const CpuUsageRecord* records, uint32_t count,
                 const std::vector<ThreadSnapshot>& /*threads*/) override {
    if (!fp_ || count == 0) return;
    fwrite(records, sizeof(CpuUsageRecord), count, fp_);
    fflush(fp_);
  }

  void OnFooter(const std::vector<ThreadSnapshot>& new_threads) override {
    if (!fp_) return;
    auto offset = static_cast<uint32_t>(ftell(fp_));
    WriteU32(kFooterMagic);
    WriteThreadMap(new_threads);
    WriteU32(offset);
    fflush(fp_);
  }

  void OnClose() override {
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
// ConsoleSink - real-time terminal display (sorted by CPU usage)
// ---------------------------------------------------------------------------
class ConsoleSink : public OutputSink {
 public:
  void OnHeader(const char* process_name, uint32_t num_cpus,
                uint32_t /*ticks_per_sec*/,
                const std::vector<ThreadSnapshot>& /*threads*/) override {
    pname_ = process_name;
    num_cpus_ = num_cpus;
  }

  void OnRecords(const CpuUsageRecord* records, uint32_t count,
                 const std::vector<ThreadSnapshot>& threads) override {
    fprintf(stdout, "\033[2J\033[H");
    fprintf(stdout, "Process: %s  CPUs: %u  Threads: %u\n\n",
            pname_, num_cpus_, count);
    fprintf(stdout, "%-8s %-20s %6s %6s %6s %4s %4s %3s\n",
            "TID", "Name", "User%", "Kern%", "Total", "CPU", "Pri", "St");
    fprintf(stdout,
            "-------- -------------------- ------ ------ ------ ---- ---- ---\n");

    // Build sort index
    sort_idx_.resize(count);
    for (uint32_t i = 0; i < count; ++i) sort_idx_[i] = i;
    std::sort(sort_idx_.begin(), sort_idx_.end(),
              [records](size_t a, size_t b) {
      return (records[a].user_percent + records[a].kernel_percent) >
             (records[b].user_percent + records[b].kernel_percent);
    });

    for (size_t idx : sort_idx_) {
      const auto& r = records[idx];
      double u = r.user_percent / 100.0;
      double k = r.kernel_percent / 100.0;
      const char* name = (idx < threads.size()) ? threads[idx].name : "?";
      fprintf(stdout, "%-8u %-20.20s %6.1f %6.1f %6.1f %4u %4d  %c\n",
              r.thread_id, name, u, k, u + k,
              static_cast<unsigned>(r.processor),
              static_cast<int>(r.priority),
              static_cast<char>(r.state));
    }
    fprintf(stdout, "\nPress Ctrl+C to stop.\n");
    fflush(stdout);
  }

  void OnFooter(const std::vector<ThreadSnapshot>& /*new_threads*/) override {}
  void OnClose() override {}

 private:
  const char* pname_ = "";
  uint32_t num_cpus_ = 0;
  std::vector<size_t> sort_idx_;
};

// ---------------------------------------------------------------------------
// ProgressSink - simple dot progress on stdout
// ---------------------------------------------------------------------------
class ProgressSink : public OutputSink {
 public:
  void OnHeader(const char* /*pname*/, uint32_t /*num_cpus*/,
                uint32_t /*tps*/,
                const std::vector<ThreadSnapshot>& /*threads*/) override {}
  void OnRecords(const CpuUsageRecord* /*records*/, uint32_t /*count*/,
                 const std::vector<ThreadSnapshot>& /*threads*/) override {
    fprintf(stdout, ".");
    fflush(stdout);
  }
  void OnFooter(const std::vector<ThreadSnapshot>& /*new_threads*/) override {}
  void OnClose() override {}
};

// ---------------------------------------------------------------------------
// CpuMonitor - scheduling and statistics only
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

    // Setup output sinks
    BinaryFileSink file_sink;
    if (!file_sink.Open(output_file_)) {
      fprintf(stderr, "Error: cannot open %s for writing\n", output_file_);
      return 1;
    }
    file_sink.OnHeader(pname, num_cpus_, ticks_per_sec_, threads_);

    ConsoleSink console_sink;
    ProgressSink progress_sink;
    OutputSink* display_sink = realtime_
        ? static_cast<OutputSink*>(&console_sink)
        : static_cast<OutputSink*>(&progress_sink);
    display_sink->OnHeader(pname, num_cpus_, ticks_per_sec_, threads_);

    records_.resize(threads_.size());

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
      SleepUntil(next_sample);

      // Measure scheduling jitter
      auto actual_time = Clock::now();
      int64_t jitter_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
          actual_time - next_sample).count();
      stats_.RecordSample(jitter_ns);

      // Detect missed sample (woke up > 2x interval late)
      if (jitter_ns > interval_ns_) {
        stats_.RecordMiss();
      }

      next_sample += std::chrono::nanoseconds(interval_ns_);

      if (duration_ns_ > 0) {
        auto elapsed = actual_time - start;
        if (elapsed >= std::chrono::nanoseconds(duration_ns_)) break;
      }

      if (!proc::IsProcessAlive(pid_)) {
        fprintf(stdout, "\nProcess %d exited.\n", pid_);
        break;
      }

      if (iteration % kThreadRescanInterval == 0) {
        ScanThreads();
        records_.resize(threads_.size());
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

      size_t nthreads = threads_.size();
      records_.resize(nthreads);
      for (size_t i = 0; i < nthreads; ++i) {
        const auto& t = threads_[i];
        uint64_t du = 0, ds = 0;
        if (i < cur_ticks_.size() && i < prev_ticks_.size() &&
            cur_ticks_[i].tid == t.tid && prev_ticks_[i].tid == t.tid) {
          du = cur_ticks_[i].utime - prev_ticks_[i].utime;
          ds = cur_ticks_[i].stime - prev_ticks_[i].stime;
        }

        auto user_pct = static_cast<uint16_t>(
            std::min(du * num_cpus_ * 10000ULL / delta_total, 10000ULL));
        auto kern_pct = static_cast<uint16_t>(
            std::min(ds * num_cpus_ * 10000ULL / delta_total, 10000ULL));

        CpuUsageRecord& rec = records_[i];
        rec.timestamp = now_epoch;
        rec.thread_id = t.tid;
        rec.user_percent = user_pct;
        rec.kernel_percent = kern_pct;
        rec.state = static_cast<uint8_t>(t.state);
        rec.processor = static_cast<uint8_t>(t.processor);
        rec.priority = static_cast<int8_t>(t.priority);
        rec.nice = static_cast<int8_t>(t.nice);
      }

      auto rec_count = static_cast<uint32_t>(records_.size());
      file_sink.OnRecords(records_.data(), rec_count, threads_);
      display_sink->OnRecords(records_.data(), rec_count, threads_);

      prev_sys_ = cur_sys;
      std::swap(prev_ticks_, cur_ticks_);
      ++iteration;
    }

    file_sink.OnFooter(new_threads_);
    file_sink.OnClose();
    display_sink->OnClose();

    PrintExitSummary();
    fprintf(stdout, "Done. %s written.\n", output_file_);
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
  proc::SystemCpuTicks prev_sys_{};
  SamplingStats stats_;
  char path_buf_[96];  // Reused path buffer

  double duration_sec_() const {
    return static_cast<double>(duration_ns_) / 1e9;
  }

  void PrintExitSummary() {
    fprintf(stdout, "\n--- Sampling Summary ---\n");
    fprintf(stdout, "  Total samples:  %u\n", stats_.total_samples);
    fprintf(stdout, "  Missed samples: %u\n", stats_.missed_samples);
    fprintf(stdout, "  Avg jitter:     %.1f us\n", stats_.AvgLatencyUs());
    fprintf(stdout, "  Max jitter:     %.1f us\n", stats_.MaxLatencyUs());
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

  // SampleTicks: fills ticks in same order as threads_ (index-aligned).
  // Entries where stat read fails get utime=stime=0 (delta will be 0).
  void SampleTicks(std::vector<TickEntry>& ticks) {
    size_t n = threads_.size();
    ticks.resize(n);
    for (size_t i = 0; i < n; ++i) {
      ticks[i].tid = threads_[i].tid;
      snprintf(path_buf_, sizeof(path_buf_),
               "/proc/%d/task/%d/stat", pid_, static_cast<int>(threads_[i].tid));
      proc::ThreadStat ts{};
      if (proc::IsOk(proc::ReadThreadStat(path_buf_, &ts))) {
        ticks[i].utime = ts.utime;
        ticks[i].stime = ts.stime;
      } else {
        ticks[i].utime = 0;
        ticks[i].stime = 0;
      }
    }
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
