// Copyright 2025 Thread Monitor Authors
// SPDX-License-Identifier: MIT
//
// proc_parser.hpp - Zero-allocation /proc filesystem parser for Linux.
//
// Design:
//   - Pure parsing separated from I/O (ParseStatFields / ParseSystemCpuLine)
//   - All public APIs return ErrorCode instead of bool
//   - Time and tick counters use uint64_t to prevent overflow
//   - Stack-only buffers, no heap allocation on hot path
//   - Google C++ Style compliant

#ifndef PROC_PARSER_HPP_
#define PROC_PARSER_HPP_

#include <dirent.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace proc {

// ---------------------------------------------------------------------------
// Error codes
// ---------------------------------------------------------------------------
enum class ErrorCode : int {
  kOk = 0,
  kFileOpenFailed = -1,
  kFileReadFailed = -2,
  kParseFailed = -3,
  kNotFound = -4,
  kInsufficientFields = -5,
};

inline bool IsOk(ErrorCode ec) { return ec == ErrorCode::kOk; }

// ---------------------------------------------------------------------------
// /proc/[pid]/stat field offsets (after splitting around parenthesized comm)
//
// Full stat line: "pid (comm) state ppid pgrp session tty_nr ..."
// After strrchr(')'), we skip state char, then parse numeric fields.
// Field indices below are 0-based offsets into the post-state numeric array.
// field[0]=ppid, field[1]=pgrp, field[2]=session, ...
//
// Kernel field 14 (utime)  -> offset 10 in our array
// Kernel field 15 (stime)  -> offset 11
// Kernel field 18 (priority)-> offset 14
// Kernel field 19 (nice)   -> offset 15
// Kernel field 39 (processor)-> offset 35
//
// Verified on x86_64 and aarch64.
// ---------------------------------------------------------------------------
static constexpr int kUtimeOffset = 10;
static constexpr int kStimeOffset = 11;
static constexpr int kPriorityOffset = 14;
static constexpr int kNiceOffset = 15;
static constexpr int kProcessorOffset = 35;
static constexpr int kMinFieldsRequired = 36;
static constexpr int kMaxStatFields = 40;

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------

// Per-thread stat data extracted from /proc/[pid]/task/[tid]/stat.
struct ThreadStat {
  uint64_t utime;      // User-mode CPU ticks
  uint64_t stime;      // Kernel-mode CPU ticks
  int32_t priority;    // Scheduling priority
  int32_t nice;        // Nice value
  int32_t processor;   // Last CPU core this thread ran on
  char state;          // Thread state: R/S/D/Z/T/t/W/X/x/K/P
};

// System-wide CPU ticks from /proc/stat (first "cpu" line).
struct SystemCpuTicks {
  uint64_t user;
  uint64_t nice;
  uint64_t system;
  uint64_t idle;
  uint64_t iowait;
  uint64_t irq;
  uint64_t softirq;
  uint64_t steal;

  uint64_t Total() const {
    return user + nice + system + idle + iowait + irq + softirq + steal;
  }

  uint64_t Active() const { return Total() - idle - iowait; }
};

// ---------------------------------------------------------------------------
// RAII guards
// ---------------------------------------------------------------------------

class DirGuard {
 public:
  explicit DirGuard(DIR* d) : dir_(d) {}
  ~DirGuard() {
    if (dir_) closedir(dir_);
  }
  DIR* Get() const { return dir_; }

  DirGuard(const DirGuard&) = delete;
  DirGuard& operator=(const DirGuard&) = delete;

 private:
  DIR* dir_;
};

class FileGuard {
 public:
  explicit FileGuard(FILE* f) : fp_(f) {}
  ~FileGuard() {
    if (fp_) fclose(fp_);
  }
  FILE* Get() const { return fp_; }

  FileGuard(const FileGuard&) = delete;
  FileGuard& operator=(const FileGuard&) = delete;

 private:
  FILE* fp_;
};

// ---------------------------------------------------------------------------
// Low-level I/O (separated from parsing)
// ---------------------------------------------------------------------------
namespace io {

// Read entire file into caller-provided buffer.
// Returns bytes read (>0) on success, 0 on error.
inline int ReadFile(const char* path, char* buf, int bufsize) {
  FILE* fp = fopen(path, "r");
  if (!fp) return 0;
  FileGuard guard(fp);
  int n = static_cast<int>(fread(buf, 1, static_cast<size_t>(bufsize - 1), fp));
  buf[n] = '\0';
  return n;
}

}  // namespace io

// ---------------------------------------------------------------------------
// Pure parsing (no I/O, testable with synthetic data)
// ---------------------------------------------------------------------------
namespace parse {

// Parse space-separated numeric fields from a string into uint64_t array.
// Handles negative values (priority/nice) by storing two's complement.
// Returns number of fields parsed.
inline int NumericFields(const char* str, uint64_t* out, int max_fields) {
  int count = 0;
  const char* p = str;
  while (*p && count < max_fields) {
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '\0' || *p == '\n') break;

    uint64_t val = 0;
    bool has_digit = false;
    bool negative = false;
    if (*p == '-') {
      negative = true;
      ++p;
    }
    while (*p >= '0' && *p <= '9') {
      val = val * 10 + static_cast<uint64_t>(*p - '0');
      ++p;
      has_digit = true;
    }
    if (has_digit) {
      out[count++] = negative
                         ? static_cast<uint64_t>(-static_cast<int64_t>(val))
                         : val;
    } else {
      // Skip non-numeric token
      while (*p && *p != ' ' && *p != '\t' && *p != '\n') ++p;
    }
  }
  return count;
}

// Parse a /proc/[pid]/stat buffer into ThreadStat.
// Input: raw content of the stat file (null-terminated).
// Returns ErrorCode.
inline ErrorCode StatBuffer(const char* buf, ThreadStat* out) {
  // Find last ')' to skip comm field (may contain spaces/parens)
  const char* last_paren = strrchr(buf, ')');
  if (!last_paren || last_paren == buf) return ErrorCode::kParseFailed;

  // State character is right after ") "
  const char* after = last_paren + 1;
  while (*after == ' ') ++after;
  out->state = *after ? *after : '?';
  ++after;

  // Parse remaining numeric fields
  uint64_t fields[kMaxStatFields] = {};
  int n = NumericFields(after, fields, kMaxStatFields);
  if (n < kMinFieldsRequired) return ErrorCode::kInsufficientFields;

  out->utime = fields[kUtimeOffset];
  out->stime = fields[kStimeOffset];
  out->priority = static_cast<int32_t>(fields[kPriorityOffset]);
  out->nice = static_cast<int32_t>(fields[kNiceOffset]);
  out->processor = static_cast<int32_t>(fields[kProcessorOffset]);
  return ErrorCode::kOk;
}

// Parse the first line of /proc/stat into SystemCpuTicks.
// Input: raw content of /proc/stat (null-terminated, at least first line).
// Returns ErrorCode.
inline ErrorCode SystemCpuLine(const char* buf, SystemCpuTicks* out) {
  // Skip "cpu" prefix
  const char* p = buf;
  while (*p && *p != ' ') ++p;

  uint64_t fields[8] = {};
  int n = NumericFields(p, fields, 8);
  if (n < 7) return ErrorCode::kInsufficientFields;

  out->user = fields[0];
  out->nice = fields[1];
  out->system = fields[2];
  out->idle = fields[3];
  out->iowait = fields[4];
  out->irq = fields[5];
  out->softirq = fields[6];
  out->steal = (n >= 8) ? fields[7] : 0;
  return ErrorCode::kOk;
}

// Parse a comm file buffer (strip trailing newline).
// Writes result into name[name_size].
inline void CommBuffer(const char* buf, int buf_len, char* name,
                       int name_size) {
  int len = buf_len;
  if (len > 0 && buf[len - 1] == '\n') --len;
  if (len >= name_size) len = name_size - 1;
  memcpy(name, buf, static_cast<size_t>(len));
  name[len] = '\0';
}

}  // namespace parse

// ---------------------------------------------------------------------------
// High-level API (I/O + parsing combined)
// ---------------------------------------------------------------------------

// Read system-wide CPU ticks from /proc/stat.
inline ErrorCode ReadSystemCpuTicks(SystemCpuTicks* out) {
  char buf[512];
  if (io::ReadFile("/proc/stat", buf, sizeof(buf)) == 0) {
    return ErrorCode::kFileOpenFailed;
  }
  return parse::SystemCpuLine(buf, out);
}

// Read /proc/[pid]/task/[tid]/stat into ThreadStat.
inline ErrorCode ReadThreadStat(const char* path, ThreadStat* out) {
  char buf[1024];
  if (io::ReadFile(path, buf, sizeof(buf)) == 0) {
    return ErrorCode::kFileOpenFailed;
  }
  return parse::StatBuffer(buf, out);
}

// Read thread name from /proc/[pid]/task/[tid]/comm.
inline ErrorCode ReadThreadName(const char* comm_path, char* name,
                                int name_size) {
  char buf[64];
  int n = io::ReadFile(comm_path, buf, sizeof(buf));
  if (n <= 0) {
    snprintf(name, static_cast<size_t>(name_size), "unknown");
    return ErrorCode::kFileOpenFailed;
  }
  parse::CommBuffer(buf, n, name, name_size);
  return ErrorCode::kOk;
}

// Read process name from /proc/[pid]/comm.
inline ErrorCode ReadProcessName(int pid, char* name, int name_size) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/comm", pid);
  return ReadThreadName(path, name, name_size);
}

// Check if a process is still alive.
inline bool IsProcessAlive(int pid) {
  char path[32];
  snprintf(path, sizeof(path), "/proc/%d", pid);
  return access(path, F_OK) == 0;
}

// Find PID by process name. Returns -1 if not found.
inline int FindPidByName(const char* process_name) {
  DIR* dir = opendir("/proc");
  if (!dir) return -1;
  DirGuard guard(dir);

  struct dirent* ent;
  while ((ent = readdir(dir)) != nullptr) {
    const char* p = ent->d_name;
    bool all_digit = true;
    while (*p) {
      if (*p < '0' || *p > '9') {
        all_digit = false;
        break;
      }
      ++p;
    }
    if (!all_digit || ent->d_name[0] == '\0') continue;

    char comm_path[280];
    snprintf(comm_path, sizeof(comm_path), "/proc/%s/comm", ent->d_name);
    char name[64];
    if (IsOk(ReadThreadName(comm_path, name, sizeof(name)))) {
      if (strcmp(name, process_name) == 0) {
        return atoi(ent->d_name);
      }
    }
  }
  return -1;
}

// Enumerate thread IDs under /proc/[pid]/task/.
// Calls callback(tid) for each thread found.
// Returns number of threads enumerated.
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
      if (*p < '0' || *p > '9') {
        all_digit = false;
        break;
      }
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

}  // namespace proc

#endif  // PROC_PARSER_HPP_
