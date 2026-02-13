# Thread Monitor

Linux 线程级 CPU 使用率监控工具。通过 `/proc` 文件系统实时采集进程内每个线程的用户态/内核态 CPU 占用率，记录为紧凑二进制格式，配合 Python 脚本进行离线分析和可视化。

## 架构

```
/proc/[pid]/task/[tid]/stat ──> cpu_monitor ──> cpu_usage.bin
                                     |
                                     └──> 实时终端显示 (-r)

cpu_usage.bin ──> cpu_usage_parser.py ──> 统计摘要 / CSV / 图表
```

## 功能

- 按线程采集用户态/内核态 CPU 使用率（精度 0.01%）
- 记录线程状态（R/S/D/Z/T）、调度优先级、运行核心
- 支持 PID 或进程名指定目标
- 进程退出自动停止监控
- 动态线程发现（运行期间新建的线程自动纳入）
- 实时终端显示模式（类 top）
- steady_clock 固定周期采样，避免漂移
- 二进制格式 v2，带 magic number 和版本号
- Python 解析器：统计摘要（avg/max/p50/p95/p99）、CSV 导出、matplotlib 图表
- 向后兼容 v1 格式

## 设计要点

- 解析层与 I/O 分离：`proc::io` 负责文件读取，`proc::parse` 负责纯解析（可用合成数据单测）
- 所有 API 返回 `ErrorCode` 枚举，不使用异常
- 热路径零堆分配：预分配 vector，复用 path buffer，flat vector 替代 std::map
- 固定宽度整数类型（uint64_t 防溢出）
- Google C++ Style，-Werror 编译
- 支持 ASan/UBSan 构建

## 性能

自身开销测试（x86_64, 64 核, 0.5s 采样间隔, 监控 9 线程进程）：

| 指标 | 值 |
|------|-----|
| cpu_monitor CPU 占用 | < 0.01% |
| 内存占用 | ~1.2 MB RSS |
| 采样精度 | 误差 < 3%（对比设定值） |

测试数据（8 worker 线程，设定 60% CPU 占用率）：

| 线程 | Avg% | Max% | P50% | P95% |
|------|------|------|------|------|
| worker_0~7 | 57.5~58.3 | 59.0~61.0 | 56.9~58.8 | 58.9~59.2 |

## 编译

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

可选 Sanitizer：

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON -DENABLE_UBSAN=ON
cmake --build .
```

要求：C++14, Linux, pthread

## 使用

### 监控

```bash
# 按 PID 监控，1秒采样，持续30秒
./cpu_monitor -p 12345 -d 1.0 -n 30

# 按进程名监控，实时显示
./cpu_monitor -p my_app -d 0.5 -r

# 自定义输出文件
./cpu_monitor -p my_app -d 1.0 -n 60 -o my_data.bin
```

参数：

| 选项 | 说明 | 默认值 |
|------|------|--------|
| `-p <PID\|name>` | 目标进程 PID 或名称 | 必填 |
| `-d <seconds>` | 采样间隔 | 1.0 |
| `-n <seconds>` | 监控总时长（0=无限） | 0 |
| `-o <file>` | 输出文件 | cpu_usage.bin |
| `-r` | 实时终端显示 | 关闭 |

### 分析

```bash
# 打印统计摘要
python3 cpu_usage_parser.py cpu_usage.bin --no-plot

# 导出 CSV
python3 cpu_usage_parser.py cpu_usage.bin --csv output.csv --no-plot

# 生成图表
python3 cpu_usage_parser.py cpu_usage.bin --plot cpu_chart.png

# 过滤特定线程，分离用户态/内核态
python3 cpu_usage_parser.py cpu_usage.bin --filter-thread worker --separate-cpu

# 只显示 Top 5 线程
python3 cpu_usage_parser.py cpu_usage.bin --top 5
```

Python 依赖：matplotlib（仅绘图需要）

### 测试工具

```bash
# 启动 4 个 worker 线程，各占 50% CPU
./dummy_worker 4 0.5

# Ctrl+C 优雅退出
```

### 单元测试

```bash
cd build && ctest --output-on-failure
```

## 二进制格式 v2

```
[Magic "CMON" 4B][Version uint32][HeaderSize uint32]
[ProcessNameLen uint32][ProcessName bytes]
[NumCPUs uint32][TicksPerSec uint32]
[ThreadMapCount uint32][{tid uint32, nameLen uint32, name bytes}...]
[CpuUsageRecord x N]  (每条 16 字节)
[Footer "CEND" 4B][NewThreadCount uint32][{tid, nameLen, name}...]
[FooterOffset uint32]
```

CpuUsageRecord 布局（16 字节，packed）：

| 字段 | 类型 | 说明 |
|------|------|------|
| timestamp | uint32 | Unix 时间戳（秒） |
| thread_id | uint32 | 线程 TID |
| user_percent | uint16 | 用户态 CPU%（0-10000 = 0-100.00%） |
| kernel_percent | uint16 | 内核态 CPU%（0-10000） |
| state | uint8 | 线程状态字符 |
| processor | uint8 | 最后运行的 CPU 核心 |
| priority | int8 | 调度优先级 |
| nice | int8 | nice 值 |

## CPU 使用率计算

```
thread_cpu% = delta_thread_ticks / (delta_system_total_ticks / num_cpus) * 100
```

- `delta_thread_ticks`：采样间隔内线程的 utime + stime 增量
- `delta_system_total_ticks`：`/proc/stat` 第一行所有字段之和的增量
- `num_cpus`：在线 CPU 核心数

单线程最大值为 100%（占满一个核心）。

## 文件结构

```
├── CMakeLists.txt         # 构建配置（-Werror, ASan/UBSan 可选）
├── CPPLINT.cfg            # cpplint 配置
├── proc_parser.hpp        # /proc 文件系统解析库（header-only, I/O 与解析分离）
├── cpu_monitor.cc         # 监控主程序（steady_clock 固定周期采样）
├── dummy_worker.cc        # 测试用负载生成器（支持 SIGINT 优雅退出）
├── cpu_usage_parser.py    # Python 解析和可视化
├── test_proc_parser.cc    # 解析层单元测试
└── README.md
```

## 平台兼容性

- x86_64 Linux: 已验证
- aarch64 (ARM) Linux: /proc/stat 字段布局一致，理论兼容

## 许可证

MIT License
