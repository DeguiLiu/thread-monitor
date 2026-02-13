/**
 * @file proc_parser.hpp
 * @brief Unified /proc filesystem parser for Linux process/thread monitoring.
 *
 * Provides zero-allocation (stack-based) parsing of:
 *   - /proc/stat           (system-wide CPU ticks)
 *   - /proc/[pid]/stat     (per-process stats)
 *   - /proc/[pid]/task/[tid]/stat  (per-thread stats)
 *   - /proc/[pid]/task/[tid]/comm  (thread name)
 */

#ifndef PROC_PARSER_HPP
#define PROC_PARSER_HPP

#include <dirent.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace proc {

// /proc/[pid]/stat field indices (0-based, after splitting around parenthesized comm)
// These are consistent across x86_64 and aarch64.
// Fields 0=pid, 1=comm, 2=state, then numbered from 2 onwards in after-paren string.
// In our parsing: fields[0]=state(idx2), fields[1]=ppid(idx3), ...
// So utime(idx13) = after_paren_fields[11], stime(idx14) = after_paren_fields[12]
// priority(idx17) = after_paren_fields[15], nice(idx18) = after_paren_fields[16]
// processor(idx38) = after_paren_fields[36]

static constexpr int kUtimeOffset   = 11;  // utime: field 13 in stat, offset 11 after ')' split
static constexpr int kStimeOffset   = 12;  // stime: field 14
static constexpr int kPriorityOffset = 15; // priority: field 17
static constexpr int kNiceOffset    = 16;  // nice: field 18
static constexpr int kNumThreadsOffset = 17; // num_threads: field 19
static constexpr int kProcessorOffset = 36;  // processor (last CPU): field 38
static constexpr int kMinFieldsRequired = 37;

/// Per-thread stat data extracted from /proc/[pid]/task/[tid]/stat
struct ThreadStat {
    uint64_t utime;       ///< User-mode CPU ticks
    uint64_t stime;       ///< Kernel-mode CPU ticks
    int32_t  priority;    ///< Scheduling priority
    int32_t  nice;        ///< Nice value
    int32_t  processor;   ///< Last CPU core this thread ran on
    char     state;       ///< Thread state: R/S/D/Z/T/t/W/X/x/K/P
};

/// System-wide CPU ticks from /proc/stat (first "cpu" line)
struct SystemCpuTicks {
    uint64_t user;
    uint64_t nice;
    uint64_t system;
    uint64_t idle;
    uint64_t iowait;
    uint64_t irq;
    uint64_t softirq;
    uint64_t steal;

    uint64_t total() const {
        return user + nice + system + idle + iowait + irq + softirq + steal;
    }

    uint64_t active() const {
        return total() - idle - iowait;
    }
};

/// RAII wrapper for DIR*
class DirGuard {
public:
    explicit DirGuard(DIR* d) : dir_(d) {}
    ~DirGuard() { if (dir_) closedir(dir_); }
    DIR* get() const { return dir_; }
    DirGuard(const DirGuard&) = delete;
    DirGuard& operator=(const DirGuard&) = delete;
private:
    DIR* dir_;
};

/// RAII wrapper for FILE*
class FileGuard {
public:
    explicit FileGuard(FILE* f) : fp_(f) {}
    ~FileGuard() { if (fp_) fclose(fp_); }
    FILE* get() const { return fp_; }
    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
private:
    FILE* fp_;
};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace detail {

/// Read entire file into caller-provided buffer. Returns bytes read, 0 on error.
inline int ReadFileToBuffer(const char* path, char* buf, int bufsize) {
    FILE* fp = fopen(path, "r");
    if (!fp) return 0;
    FileGuard guard(fp);
    int n = static_cast<int>(fread(buf, 1, static_cast<size_t>(bufsize - 1), fp));
    buf[n] = '\0';
    return n;
}

/// Parse space-separated uint64 fields from a string.
/// Returns number of fields parsed.
inline int ParseUint64Fields(const char* str, uint64_t* out, int max_fields) {
    int count = 0;
    const char* p = str;
    while (*p && count < max_fields) {
        // Skip whitespace
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '\0' || *p == '\n') break;

        uint64_t val = 0;
        bool has_digit = false;
        bool negative = false;
        if (*p == '-') { negative = true; ++p; }
        while (*p >= '0' && *p <= '9') {
            val = val * 10 + static_cast<uint64_t>(*p - '0');
            ++p;
            has_digit = true;
        }
        if (has_digit) {
            // Store as uint64_t; negative values (like nice/priority) will be
            // reinterpreted by caller via static_cast<int32_t>
            out[count++] = negative ? static_cast<uint64_t>(-static_cast<int64_t>(val)) : val;
        } else {
            // Skip non-numeric token
            while (*p && *p != ' ' && *p != '\t' && *p != '\n') ++p;
        }
    }
    return count;
}

} // namespace detail

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// Read system-wide CPU ticks from /proc/stat.
/// Returns true on success.
inline bool ReadSystemCpuTicks(SystemCpuTicks& out) {
    char buf[512];
    if (detail::ReadFileToBuffer("/proc/stat", buf, sizeof(buf)) == 0) {
        return false;
    }

    // First line: "cpu  user nice system idle iowait irq softirq steal ..."
    // Skip "cpu" prefix
    const char* p = buf;
    while (*p && *p != ' ') ++p;

    uint64_t fields[8] = {};
    int n = detail::ParseUint64Fields(p, fields, 8);
    if (n < 7) return false;

    out.user    = fields[0];
    out.nice    = fields[1];
    out.system  = fields[2];
    out.idle    = fields[3];
    out.iowait  = fields[4];
    out.irq     = fields[5];
    out.softirq = fields[6];
    out.steal   = (n >= 8) ? fields[7] : 0;
    return true;
}

/// Parse /proc/[pid]/task/[tid]/stat into ThreadStat.
/// `path` should be the full path to the stat file.
/// Returns true on success.
inline bool ReadThreadStat(const char* path, ThreadStat& out) {
    char buf[1024];
    if (detail::ReadFileToBuffer(path, buf, sizeof(buf)) == 0) {
        return false;
    }

    // Find the comm field enclosed in parentheses: "pid (comm) state ..."
    // The comm field may contain spaces and parentheses, so find last ')'.
    const char* last_paren = strrchr(buf, ')');
    if (!last_paren || last_paren == buf) return false;

    // State character is right after ") "
    const char* after = last_paren + 1;
    while (*after == ' ') ++after;
    out.state = *after ? *after : '?';
    ++after;

    // Parse remaining numeric fields
    uint64_t fields[40] = {};
    int n = detail::ParseUint64Fields(after, fields, 40);
    if (n < kMinFieldsRequired) return false;

    out.utime    = fields[kUtimeOffset];
    out.stime    = fields[kStimeOffset];
    out.priority = static_cast<int32_t>(fields[kPriorityOffset]);
    out.nice     = static_cast<int32_t>(fields[kNiceOffset]);
    out.processor = static_cast<int32_t>(fields[kProcessorOffset]);
    return true;
}

/// Read thread name from /proc/[pid]/task/[tid]/comm.
/// Writes into `name` buffer of size `name_size`. Returns true on success.
inline bool ReadThreadName(const char* comm_path, char* name, int name_size) {
    char buf[64];
    int n = detail::ReadFileToBuffer(comm_path, buf, sizeof(buf));
    if (n <= 0) {
        snprintf(name, static_cast<size_t>(name_size), "unknown");
        return false;
    }
    // Strip trailing newline
    if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
    snprintf(name, static_cast<size_t>(name_size), "%s", buf);
    return true;
}

/// Read process name from /proc/[pid]/comm.
inline bool ReadProcessName(int pid, char* name, int name_size) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    return ReadThreadName(path, name, name_size);
}

/// Check if a process is still alive.
inline bool IsProcessAlive(int pid) {
    char path[32];
    snprintf(path, sizeof(path), "/proc/%d", pid);
    return access(path, F_OK) == 0;
}

/// Find PID by process name. Returns -1 if not found.
inline int FindPidByName(const char* process_name) {
    DIR* dir = opendir("/proc");
    if (!dir) return -1;
    DirGuard guard(dir);

    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        // Only check numeric directories
        const char* p = ent->d_name;
        bool all_digit = true;
        while (*p) {
            if (*p < '0' || *p > '9') { all_digit = false; break; }
            ++p;
        }
        if (!all_digit || ent->d_name[0] == '\0') continue;

        char comm_path[64];
        snprintf(comm_path, sizeof(comm_path), "/proc/%s/comm", ent->d_name);
        char name[64];
        if (ReadThreadName(comm_path, name, sizeof(name))) {
            if (strcmp(name, process_name) == 0) {
                return atoi(ent->d_name);
            }
        }
    }
    return -1;
}

/// Enumerate thread IDs under /proc/[pid]/task/.
/// Calls `callback(tid)` for each thread found.
/// Returns number of threads enumerated.
template <typename Callback>
int EnumerateThreads(int pid, Callback callback) {
    char task_path[64];
    snprintf(task_path, sizeof(task_path), "/proc/%d/task", pid);

    DIR* dir = opendir(task_path);
    if (!dir) return 0;
    DirGuard guard(dir);

    int count = 0;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        const char* p = ent->d_name;
        bool all_digit = true;
        int tid = 0;
        while (*p) {
            if (*p < '0' || *p > '9') { all_digit = false; break; }
            tid = tid * 10 + (*p - '0');
            ++p;
        }
        if (all_digit && ent->d_name[0] != '\0') {
            callback(tid);
            ++count;
        }
    }
    return count;
}

} // namespace proc

#endif // PROC_PARSER_HPP
