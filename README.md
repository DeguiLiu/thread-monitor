## 概述

本工具是一个结合 C++ 和 Python 的 CPU 使用率监控与分析工具。通过读取 Linux 系统的 `/proc` 文件系统，实时采集 CPU 使用数据。C++程序负责采集CPU数据并记录到二进制文件中，Python脚本则解析这些数据并生成直观的可视化图表

## 功能特性

- **实时监控**：实时采集指定进程及其线程的用户态和内核态CPU使用率。
- **多线程支持**：自动识别并监控进程中的所有线程，获取每个线程的CPU使用数据。
- **数据记录**：将采集到的CPU使用数据以高效的二进制格式存储，便于后续分析。
- **可视化分析**：通过Python脚本解析数据并生成详细的CPU使用率图表，支持过滤特定线程和时间范围。
- **优雅退出**：支持通过`Ctrl+C`中断程序，确保数据完整性和资源的正确释放。

## 安装与使用

### C++录制程序

#### 编译

确保系统已安装`g++`编译器，然后在项目根目录下运行：

```bash
g++ -std=c++11 -o cpu_monitor cpu_monitor.cc -pthread
```

#### 运行

```bash
./cpu_monitor [选项] <PID或进程名称>
```

**选项**：

- `-h`：显示帮助信息。
- `-n <延迟>`：设置CPU使用数据刷新间隔（秒，支持小数，如0.1）。
- `-o <输出文件>`：设置输出二进制文件名（默认`cpu_usage.bin`）。

### Python解析脚本

#### 安装依赖

确保已安装Python 3及以下库：

```bash
pip install pandas matplotlib
```

#### 运行

```bash
python3 cpu_usage_parser cpu_usage.bin [选项]
```

**选项**：

- `--filter-thread <线程名>`：按线程名称过滤数据。
- `--filter-cpu-type {user,kernel}`：按CPU使用类型过滤数据。
- `--time-range "开始时间,结束时间"`：指定时间范围（格式如`2024-09-24 12:00:00,2024-09-24 12:30:00`）。
- `--hide-summary`：隐藏图表底部的摘要信息。

## 示例

1. 监控进程ID为`12345`，刷新间隔为`0.5`秒，输出文件为`cpu_data.bin`：

   ```bash
   ./cpu_monitor -n 0.5 -o cpu_data.bin 12345
   ```

2. 解析并绘制数据，过滤线程名包含`worker`的线程：

   ```bash
   python3 cpu_usage_parser cpu_data.bin --filter-thread worker
   ```

## 贡献

欢迎任何形式的贡献！请提交Issue或Pull Request，以帮助我们改进项目。

## 许可证

本项目采用MIT许可证。详情请参阅[LICENSE](LICENSE)。
