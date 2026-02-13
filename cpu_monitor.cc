/**
 * @file cpu_monitor.cc
 * @brief Per-thread CPU usage monitor for Linux processes.
 *
 * Reads /proc filesystem to sample per-thread CPU ticks, computes usage
 * percentages, writes binary records, and optionally displays real-time
 * terminal output.
 *
 * Binary file format v2:
 *   [Magic "CMON" 4B][Version uint32][HeaderSize uint32]
 *   [ProcessNameLen uint32][ProcessName bytes]
 *   [NumCPUs uint32][TicksPerSec uint32]
 *   [ThreadMapCount uint32][{tid uint32, nameLen uint32, name bytes}...]
 *   [CpuUsageRecord...]
 *   [Magic "CEND" 4B][NewThreadCount uint32][{tid,nameLen,name}...]
 *   [FooterOffset uint32]
 */

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
#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Binary record layout (packed, 18 bytes)
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct CpuUsageRecord {
    uint32_t timestamp;      // seconds since epoch
    uint32_t thread_id;      // TID
    uint16_t user_percent;   // 0-10000 (100.00%)
    uint16_t kernel_percent; // 0-10000 (100.00%)
    uint8_t  state;          // thread state char
    uint8_t  processor;      // last CPU core
    int8_t   priority;       // scheduling priority
    int8_t   nice;           // nice value
};
#pragma pack(pop)

static_assert(sizeof(CpuUsageRecord) == 16, "CpuUsageRecord must be 16 bytes");

// ---------------------------------------------------------------------------
// File format constants
// ---------------------------------------------------------------------------
static constexpr uint32_t kMagic       = 0x4E4F4D43; // "CMON" little-endian
static constexpr uint32_t kVersion     = 2;
static constexpr uint32_t kFooterMagic = 0x444E4543; // "CEND" little-endian

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
// Thread snapshot
// ---------------------------------------------------------------------------
struct ThreadSnapshot {
    uint32_t tid;
    char     name[32];
    uint64_t utime;
    uint64_t stime;
    char     state;
    int32_t  priority;
    int32_t  nice;
    int32_t  processor;
};

// ---------------------------------------------------------------------------
// BinaryWriter
// ---------------------------------------------------------------------------
class BinaryWriter {
public:
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

        // Placeholder for header_size
        long header_size_pos = ftell(fp_);
        WriteU32(0);
        long header_start = ftell(fp_);

        // Process name
        auto pname_len = static_cast<uint32_t>(strlen(process_name));
        WriteU32(pname_len);
        fwrite(process_name, 1, pname_len, fp_);

        // System info
        WriteU32(num_cpus);
        WriteU32(ticks_per_sec);

        // Thread map
        WriteThreadMap(threads);

        // Patch header_size
        long header_end = ftell(fp_);
        auto header_size = static_cast<uint32_t>(header_end - header_start);
        fseek(fp_, header_size_pos, SEEK_SET);
        WriteU32(header_size);
        fseek(fp_, header_end, SEEK_SET);
        fflush(fp_);
    }

    void WriteRecords(const CpuUsageRecord* records, size_t count) {
        if (!fp_ || count == 0) return;
        fwrite(records, sizeof(CpuUsageRecord), count, fp_);
        fflush(fp_);
    }

    void WriteFooter(const std::vector<ThreadSnapshot>& new_threads) {
        if (!fp_) return;
        auto footer_offset = static_cast<uint32_t>(ftell(fp_));
        WriteU32(kFooterMagic);
        WriteThreadMap(new_threads);
        WriteU32(footer_offset);
        fflush(fp_);
    }

    void Close() {
        if (fp_) { fclose(fp_); fp_ = nullptr; }
    }

    ~BinaryWriter() { Close(); }

    BinaryWriter() = default;
    BinaryWriter(const BinaryWriter&) = delete;
    BinaryWriter& operator=(const BinaryWriter&) = delete;

private:
    FILE* fp_ = nullptr;

    void WriteU32(uint32_t val) {
        fwrite(&val, sizeof(val), 1, fp_);
    }

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
// CpuMonitor
// ---------------------------------------------------------------------------
class CpuMonitor {
public:
    CpuMonitor(int pid, double interval, double duration,
               const char* output, bool realtime)
        : pid_(pid)
        , interval_(interval)
        , duration_(duration)
        , output_file_(output)
        , realtime_(realtime)
        , ticks_per_sec_(static_cast<uint32_t>(sysconf(_SC_CLK_TCK)))
        , num_cpus_(static_cast<uint32_t>(sysconf(_SC_NPROCESSORS_ONLN))) {}

    int Run() {
        char pname[64];
        if (!proc::ReadProcessName(pid_, pname, sizeof(pname))) {
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

        fprintf(stdout, "Monitoring: %s (PID %d), %u threads, %u CPUs\n",
                pname, pid_,
                static_cast<unsigned>(threads_.size()), num_cpus_);
        fprintf(stdout, "Interval: %.2fs, Duration: %s, Output: %s\n",
                interval_,
                duration_ > 0 ? (std::to_string(duration_) + "s").c_str()
                               : "unlimited",
                output_file_);

        // Initial sample
        proc::ReadSystemCpuTicks(prev_sys_);
        SampleThreadTicks(prev_ticks_);

        auto start = std::chrono::steady_clock::now();
        uint32_t iteration = 0;

        while (g_running.load(std::memory_order_relaxed)) {
            SleepMs(static_cast<int>(interval_ * 1000.0));

            if (duration_ > 0) {
                double secs = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - start).count();
                if (secs >= duration_) break;
            }

            if (!proc::IsProcessAlive(pid_)) {
                fprintf(stdout, "\nProcess %d exited.\n", pid_);
                break;
            }

            // Rescan threads every 5 iterations
            if (iteration % 5 == 0) {
                ScanThreads();
            }

            proc::SystemCpuTicks cur_sys{};
            proc::ReadSystemCpuTicks(cur_sys);

            std::map<uint32_t, std::pair<uint64_t, uint64_t>> cur_ticks;
            SampleThreadTicks(cur_ticks);

            uint64_t delta_total = cur_sys.total() - prev_sys_.total();
            if (delta_total == 0) {
                prev_sys_ = cur_sys;
                prev_ticks_ = cur_ticks;
                ++iteration;
                continue;
            }

            auto now_epoch = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());

            std::vector<CpuUsageRecord> records;
            records.reserve(threads_.size());

            for (const auto& t : threads_) {
                uint64_t du = 0, ds = 0;
                auto cit = cur_ticks.find(t.tid);
                auto pit = prev_ticks_.find(t.tid);
                if (cit != cur_ticks.end() && pit != prev_ticks_.end()) {
                    du = cit->second.first  - pit->second.first;
                    ds = cit->second.second - pit->second.second;
                }

                // CPU% = delta_ticks / (delta_total / num_cpus) * 100
                //      = delta_ticks * num_cpus * 10000 / delta_total
                auto user_pct = static_cast<uint16_t>(
                    std::min(du * num_cpus_ * 10000ULL / delta_total, 10000ULL));
                auto kern_pct = static_cast<uint16_t>(
                    std::min(ds * num_cpus_ * 10000ULL / delta_total, 10000ULL));

                CpuUsageRecord rec{};
                rec.timestamp      = now_epoch;
                rec.thread_id      = t.tid;
                rec.user_percent   = user_pct;
                rec.kernel_percent = kern_pct;
                rec.state          = static_cast<uint8_t>(t.state);
                rec.processor      = static_cast<uint8_t>(t.processor);
                rec.priority       = static_cast<int8_t>(t.priority);
                rec.nice           = static_cast<int8_t>(t.nice);
                records.push_back(rec);
            }

            writer_.WriteRecords(records.data(), records.size());

            if (realtime_) {
                PrintRealtime(records, pname);
            } else {
                fprintf(stdout, ".");
                fflush(stdout);
            }

            prev_sys_ = cur_sys;
            prev_ticks_ = cur_ticks;
            ++iteration;
        }

        writer_.WriteFooter(new_threads_);
        writer_.Close();
        fprintf(stdout, "\nDone. %s written.\n", output_file_);
        return 0;
    }

private:
    int pid_;
    double interval_;
    double duration_;
    const char* output_file_;
    bool realtime_;
    uint32_t ticks_per_sec_;
    uint32_t num_cpus_;

    std::vector<ThreadSnapshot> threads_;
    std::vector<ThreadSnapshot> new_threads_;
    std::map<uint32_t, bool> known_tids_;
    proc::SystemCpuTicks prev_sys_{};
    std::map<uint32_t, std::pair<uint64_t, uint64_t>> prev_ticks_;
    BinaryWriter writer_;

    void ScanThreads() {
        threads_.clear();
        proc::EnumerateThreads(pid_, [this](int tid) {
            ThreadSnapshot snap{};
            snap.tid = static_cast<uint32_t>(tid);

            char comm_path[96];
            snprintf(comm_path, sizeof(comm_path),
                     "/proc/%d/task/%d/comm", pid_, tid);
            proc::ReadThreadName(comm_path, snap.name, sizeof(snap.name));

            char stat_path[96];
            snprintf(stat_path, sizeof(stat_path),
                     "/proc/%d/task/%d/stat", pid_, tid);
            proc::ThreadStat ts{};
            if (proc::ReadThreadStat(stat_path, ts)) {
                snap.utime     = ts.utime;
                snap.stime     = ts.stime;
                snap.state     = ts.state;
                snap.priority  = ts.priority;
                snap.nice      = ts.nice;
                snap.processor = ts.processor;
            }

            threads_.push_back(snap);

            if (known_tids_.find(snap.tid) == known_tids_.end()) {
                if (!known_tids_.empty()) {
                    new_threads_.push_back(snap);
                }
                known_tids_[snap.tid] = true;
            }
        });
    }

    void SampleThreadTicks(
            std::map<uint32_t, std::pair<uint64_t, uint64_t>>& ticks) {
        ticks.clear();
        for (const auto& t : threads_) {
            char path[96];
            snprintf(path, sizeof(path),
                     "/proc/%d/task/%d/stat", pid_, static_cast<int>(t.tid));
            proc::ThreadStat ts{};
            if (proc::ReadThreadStat(path, ts)) {
                ticks[t.tid] = {ts.utime, ts.stime};
            }
        }
    }

    void PrintRealtime(const std::vector<CpuUsageRecord>& records,
                       const char* pname) {
        fprintf(stdout, "\033[2J\033[H");
        fprintf(stdout, "Process: %s (PID %d)  CPUs: %u  Interval: %.1fs\n\n",
                pname, pid_, num_cpus_, interval_);
        fprintf(stdout, "%-8s %-20s %6s %6s %6s %4s %4s %3s\n",
                "TID", "Name", "User%", "Kern%", "Total", "CPU", "Pri", "St");
        fprintf(stdout,
                "-------- -------------------- ------ ------ ------ ---- ---- ---\n");

        // Sort by total CPU descending
        std::vector<size_t> idx(records.size());
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
        std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
            return (records[a].user_percent + records[a].kernel_percent) >
                   (records[b].user_percent + records[b].kernel_percent);
        });

        for (size_t i : idx) {
            const auto& r = records[i];
            double u = r.user_percent / 100.0;
            double k = r.kernel_percent / 100.0;
            const char* name = "?";
            for (const auto& t : threads_) {
                if (t.tid == r.thread_id) { name = t.name; break; }
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

    static void SleepMs(int ms) {
        struct timespec ts;
        ts.tv_sec  = ms / 1000;
        ts.tv_nsec = (ms % 1000) * 1000000L;
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
        "  -h              Show this help\n", prog);
}

static bool ParseArgs(int argc, char* argv[], Options& opts) {
    int opt;
    const char* pid_or_name = nullptr;

    while ((opt = getopt(argc, argv, "hp:o:n:d:r")) != -1) {
        switch (opt) {
        case 'h': PrintUsage(argv[0]); return false;
        case 'p': pid_or_name = optarg; break;
        case 'o': opts.output = optarg; break;
        case 'r': opts.realtime = true; break;
        case 'n':
            opts.duration = atof(optarg);
            if (opts.duration < 0) {
                fprintf(stderr, "Error: duration must be >= 0\n");
                return false;
            }
            break;
        case 'd':
            opts.interval = atof(optarg);
            if (opts.interval <= 0) {
                fprintf(stderr, "Error: interval must be > 0\n");
                return false;
            }
            break;
        default: PrintUsage(argv[0]); return false;
        }
    }

    if (!pid_or_name) {
        fprintf(stderr, "Error: -p <PID|name> is required.\n");
        PrintUsage(argv[0]);
        return false;
    }

    bool is_numeric = true;
    for (const char* p = pid_or_name; *p; ++p) {
        if (*p < '0' || *p > '9') { is_numeric = false; break; }
    }

    if (is_numeric) {
        opts.pid = atoi(pid_or_name);
    } else {
        opts.pid = proc::FindPidByName(pid_or_name);
        if (opts.pid < 0) {
            fprintf(stderr, "Error: process '%s' not found.\n", pid_or_name);
            return false;
        }
        fprintf(stdout, "Found process '%s' with PID %d\n",
                pid_or_name, opts.pid);
    }

    if (!proc::IsProcessAlive(opts.pid)) {
        fprintf(stderr, "Error: PID %d does not exist.\n", opts.pid);
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::signal(SIGINT,  SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    Options opts;
    if (!ParseArgs(argc, argv, opts)) {
        return 1;
    }

    CpuMonitor monitor(opts.pid, opts.interval, opts.duration,
                        opts.output, opts.realtime);
    return monitor.Run();
}
