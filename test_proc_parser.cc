// Copyright 2025 Thread Monitor Authors
// SPDX-License-Identifier: MIT
//
// test_proc_parser.cc - Unit tests for proc::parse namespace.
//
// Minimal test framework (no external dependencies). Tests pure parsing
// functions with synthetic data including edge cases and malformed input.

#include "proc_parser.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>

static int g_passed = 0;
static int g_failed = 0;

#define TEST(name)                                            \
  static void test_##name();                                  \
  static struct Register_##name {                             \
    Register_##name() { test_##name(); }                      \
  } reg_##name;                                               \
  static void test_##name()

#define EXPECT_EQ(a, b)                                       \
  do {                                                        \
    if ((a) == (b)) {                                         \
      ++g_passed;                                             \
    } else {                                                  \
      fprintf(stderr, "FAIL %s:%d: %s != %s\n",              \
              __FILE__, __LINE__, #a, #b);                    \
      ++g_failed;                                             \
    }                                                         \
  } while (0)

#define EXPECT_TRUE(x) EXPECT_EQ(!!(x), true)
#define EXPECT_FALSE(x) EXPECT_EQ(!!(x), false)

// ---------------------------------------------------------------------------
// parse::NumericFields
// ---------------------------------------------------------------------------
TEST(NumericFields_basic) {
  uint64_t fields[8] = {};
  int n = proc::parse::NumericFields("10 20 30", fields, 8);
  EXPECT_EQ(n, 3);
  EXPECT_EQ(fields[0], 10ULL);
  EXPECT_EQ(fields[1], 20ULL);
  EXPECT_EQ(fields[2], 30ULL);
}

TEST(NumericFields_negative) {
  uint64_t fields[4] = {};
  int n = proc::parse::NumericFields("-5 100 -20", fields, 4);
  EXPECT_EQ(n, 3);
  EXPECT_EQ(static_cast<int64_t>(fields[0]), -5);
  EXPECT_EQ(fields[1], 100ULL);
  EXPECT_EQ(static_cast<int64_t>(fields[2]), -20);
}

TEST(NumericFields_empty) {
  uint64_t fields[4] = {};
  int n = proc::parse::NumericFields("", fields, 4);
  EXPECT_EQ(n, 0);
}

TEST(NumericFields_whitespace_only) {
  uint64_t fields[4] = {};
  int n = proc::parse::NumericFields("   \t  \n", fields, 4);
  EXPECT_EQ(n, 0);
}

TEST(NumericFields_max_limit) {
  uint64_t fields[2] = {};
  int n = proc::parse::NumericFields("1 2 3 4 5", fields, 2);
  EXPECT_EQ(n, 2);
  EXPECT_EQ(fields[0], 1ULL);
  EXPECT_EQ(fields[1], 2ULL);
}

TEST(NumericFields_large_values) {
  uint64_t fields[2] = {};
  int n = proc::parse::NumericFields("18446744073709551615 0", fields, 2);
  EXPECT_EQ(n, 2);
  EXPECT_EQ(fields[0], UINT64_MAX);
}

TEST(NumericFields_tabs_and_spaces) {
  uint64_t fields[4] = {};
  int n = proc::parse::NumericFields("  42\t\t99  7 ", fields, 4);
  EXPECT_EQ(n, 3);
  EXPECT_EQ(fields[0], 42ULL);
  EXPECT_EQ(fields[1], 99ULL);
  EXPECT_EQ(fields[2], 7ULL);
}

// ---------------------------------------------------------------------------
// parse::StatBuffer
// ---------------------------------------------------------------------------

// Realistic /proc/[pid]/stat line (fields after comm truncated for brevity,
// but enough to cover kProcessorOffset = 36).
// Format: "pid (comm) state ppid pgrp sess tty tpgid flags minflt cminflt
//          majflt cmajflt utime stime cutime cstime priority nice num_threads
//          itrealvalue starttime vsize rss rsslim startcode endcode startstack
//          kstkesp kstkeip signal blocked sigignore sigcatch wchan nswap
//          cnswap exit_signal processor ..."
static const char* kStatLine =
    "12345 (test_proc) S 1 12345 12345 0 -1 4194304 "
    "100 0 0 0 "       // minflt cminflt majflt cmajflt
    "5000 1200 "        // utime(13) stime(14)
    "0 0 "              // cutime cstime
    "20 0 "             // priority(17) nice(18)
    "4 "                // num_threads
    "0 12345678 "       // itrealvalue starttime
    "1000000 500 "      // vsize rss
    "18446744073709551615 " // rsslim
    "0 0 0 0 0 "        // startcode endcode startstack kstkesp kstkeip
    "0 0 0 0 "          // signal blocked sigignore sigcatch
    "0 0 0 "            // wchan nswap cnswap
    "17 "               // exit_signal
    "3 "                // processor(38)
    "0 0 0";            // rt_priority policy ...

TEST(StatBuffer_normal) {
  proc::ThreadStat ts{};
  auto ec = proc::parse::StatBuffer(kStatLine, &ts);
  EXPECT_TRUE(proc::IsOk(ec));
  EXPECT_EQ(ts.state, 'S');
  EXPECT_EQ(ts.utime, 5000ULL);
  EXPECT_EQ(ts.stime, 1200ULL);
  EXPECT_EQ(ts.priority, 20);
  EXPECT_EQ(ts.nice, 0);
  EXPECT_EQ(ts.processor, 3);
}

TEST(StatBuffer_negative_nice) {
  // comm with spaces and parens: "(tricky (name))"
  const char* line =
      "999 (tricky (name)) R 1 999 999 0 -1 0 "
      "0 0 0 0 "
      "100 50 "
      "0 0 "
      "-10 -5 "  // priority=-10, nice=-5
      "1 0 0 0 0 "
      "18446744073709551615 "
      "0 0 0 0 0 "
      "0 0 0 0 "
      "0 0 0 "
      "17 "
      "7 "
      "0 0 0";
  proc::ThreadStat ts{};
  auto ec = proc::parse::StatBuffer(line, &ts);
  EXPECT_TRUE(proc::IsOk(ec));
  EXPECT_EQ(ts.state, 'R');
  EXPECT_EQ(ts.utime, 100ULL);
  EXPECT_EQ(ts.stime, 50ULL);
  EXPECT_EQ(ts.priority, -10);
  EXPECT_EQ(ts.nice, -5);
  EXPECT_EQ(ts.processor, 7);
}

TEST(StatBuffer_no_paren) {
  proc::ThreadStat ts{};
  auto ec = proc::parse::StatBuffer("12345 no_paren S 1 2 3", &ts);
  EXPECT_EQ(ec, proc::ErrorCode::kParseFailed);
}

TEST(StatBuffer_empty) {
  proc::ThreadStat ts{};
  auto ec = proc::parse::StatBuffer("", &ts);
  EXPECT_EQ(ec, proc::ErrorCode::kParseFailed);
}

TEST(StatBuffer_insufficient_fields) {
  // Only a few fields after comm
  proc::ThreadStat ts{};
  auto ec = proc::parse::StatBuffer("1 (x) S 1 2 3", &ts);
  EXPECT_EQ(ec, proc::ErrorCode::kInsufficientFields);
}

// ---------------------------------------------------------------------------
// parse::SystemCpuLine
// ---------------------------------------------------------------------------
TEST(SystemCpuLine_normal) {
  const char* line =
      "cpu  10000 500 3000 80000 200 100 50 10 0 0\n"
      "cpu0 5000 250 1500 40000 100 50 25 5 0 0\n";
  proc::SystemCpuTicks ticks{};
  auto ec = proc::parse::SystemCpuLine(line, &ticks);
  EXPECT_TRUE(proc::IsOk(ec));
  EXPECT_EQ(ticks.user, 10000ULL);
  EXPECT_EQ(ticks.nice, 500ULL);
  EXPECT_EQ(ticks.system, 3000ULL);
  EXPECT_EQ(ticks.idle, 80000ULL);
  EXPECT_EQ(ticks.iowait, 200ULL);
  EXPECT_EQ(ticks.irq, 100ULL);
  EXPECT_EQ(ticks.softirq, 50ULL);
  EXPECT_EQ(ticks.steal, 10ULL);
  EXPECT_EQ(ticks.Total(), 93860ULL);
}

TEST(SystemCpuLine_7fields) {
  // Older kernels may not have steal
  const char* line = "cpu  1000 200 300 5000 100 50 25\n";
  proc::SystemCpuTicks ticks{};
  auto ec = proc::parse::SystemCpuLine(line, &ticks);
  EXPECT_TRUE(proc::IsOk(ec));
  EXPECT_EQ(ticks.steal, 0ULL);
  EXPECT_EQ(ticks.Total(), 6675ULL);
}

TEST(SystemCpuLine_insufficient) {
  const char* line = "cpu  100 200 300";
  proc::SystemCpuTicks ticks{};
  auto ec = proc::parse::SystemCpuLine(line, &ticks);
  EXPECT_EQ(ec, proc::ErrorCode::kInsufficientFields);
}

// ---------------------------------------------------------------------------
// parse::CommBuffer
// ---------------------------------------------------------------------------
TEST(CommBuffer_normal) {
  char name[32];
  proc::parse::CommBuffer("my_thread\n", 10, name, sizeof(name));
  EXPECT_EQ(strcmp(name, "my_thread"), 0);
}

TEST(CommBuffer_no_newline) {
  char name[32];
  proc::parse::CommBuffer("worker_0", 8, name, sizeof(name));
  EXPECT_EQ(strcmp(name, "worker_0"), 0);
}

TEST(CommBuffer_truncate) {
  char name[4];
  proc::parse::CommBuffer("longname\n", 9, name, sizeof(name));
  EXPECT_EQ(strcmp(name, "lon"), 0);
}

TEST(CommBuffer_empty) {
  char name[16];
  proc::parse::CommBuffer("", 0, name, sizeof(name));
  EXPECT_EQ(strcmp(name, ""), 0);
}

// ---------------------------------------------------------------------------
// SystemCpuTicks::Active
// ---------------------------------------------------------------------------
TEST(SystemCpuTicks_Active) {
  proc::SystemCpuTicks t{};
  t.user = 100;
  t.nice = 10;
  t.system = 50;
  t.idle = 800;
  t.iowait = 20;
  t.irq = 5;
  t.softirq = 3;
  t.steal = 2;
  EXPECT_EQ(t.Total(), 990ULL);
  EXPECT_EQ(t.Active(), 170ULL);  // Total - idle - iowait
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
  fprintf(stdout, "\nproc_parser unit tests: %d passed, %d failed\n",
          g_passed, g_failed);
  return g_failed > 0 ? 1 : 0;
}
