设计方案概述
本方案的目标是在不同的硬件平台上（如 Ubuntu x86 和 RK3566）监测进程和线程的 CPU 使用情况。它包括数据采集、处理与存储，以及使用 Python 进行数据分析和可视化。
代码位置： https://git.zh.midea.com/liudg16/thread-monitor
数据采集
- Linux 的 /proc 文件系统:
  - 使用 /proc 文件系统来获取进程和线程的详细信息，如 PID、线程 ID、状态、CPU 时间等。
  - 例如，在 Ubuntu x86 上，可以使用 cat /proc/27562/stat 获取进程的信息，而 cat /proc/27562/task/27563/stat 可用于获取线程的信息。
  - 在 RK3566 平台上，类似的命令是 cat /proc/1073/stat 和 cat /proc/1073/task/1081/stat。
- C++ 定时器/循环:
  - 开发一个 C++ 程序，使用定时器或循环定期读取 /proc 文件中的信息。
  - 程序需要考虑到不同架构下字段位置的不同，例如，RK3566 上的优先级和 nice 值在第 19 和 20 个字段，而 Ubuntu x86 则在第 18 和 19 个字段。
数据处理与存储
- 数据处理:
  - 使用 ParseStatFile 函数来解析 /proc/[pid]/task/[tid]/stat 文件，从中提取用户态时间 (utime) 和内核态时间 (stime)。
  - 通过 PrintProcessInfo 函数从 stat 文件中提取优先级、nice 值等信息，并打印到控制台和文件中。
  - 使用 ComputeCpuUsage 函数来计算每个线程的 CPU 使用率。
- 数据存储:
  - 将处理后的 CPU 使用率以紧凑的格式记录到文本文件中，节省存储空间。
  - 当文件大小超过 10MB 时，使用 RotateLogFileIfNeeded 函数进行日志轮换，创建新的日志文件。
Python 解析脚本
- 数据聚合:
  - 为了解决具有相同名称的多个线程问题，在数据处理阶段将这些线程的 CPU 使用量相加。
  - 使用 Python 字典 temp_data 来聚合具有相同名称的线程的 CPU 使用量。
- 统计与绘图:
  - 使用 print_statistics 函数来显示统计信息，包括最大和平均的用户态和内核态 CPU 使用率。
  - 使用 plot_cpu_usage 函数来绘制进程和线程的 CPU 使用曲线。
  - 进程的总 CPU 使用曲线用黑色加粗显示，以便与各线程的曲线区分开来。
扩展功能
- 进程总 CPU 使用情况:
  - 使用 calculate_process_cpu 函数计算所有线程的 CPU 使用量汇总，得出进程的总 CPU 使用情况。
  - 在绘图时，首先绘制整个进程的总 CPU 使用情况曲线，并将其显示在最上方。
- 日志管理:
  - 在日志轮转时，使用 RotateLogFileIfNeeded 函数重新打印基本信息到新创建的日志文件的开头。
  - 为确保信息完整，修改后的 PrintProcessInfo 函数会在程序启动时立即打印进程和线程的基本信息到控制台和日志文件中。
执行结果示例
以下是 Python 解析脚本执行的一个示例结果：

$ sudo python3 cpu_read.py /media/sf_shared/process_1040.csv
Process ID: 1040 | Prio: 0
planner_node: Prio=0 | Max/Avg User=8.0%/0.57% | Max/Avg Kernel=1.0%/0.83%
MotionManager: Prio=0 | Max/Avg User=2.0%/1.10% | Max/Avg Kernel=0.0%/0.00%
SensorPreproces: Prio=0 | Max/Avg User=11.0%/5.51% | Max/Avg Kernel=0.0%/0.00%
LineLaserLocalM: Prio=0 | Max/Avg User=1.0%/1.00% | Max/Avg Kernel=0.0%/0.00%
PlannerStub: Prio=0 | Max/Avg User=1.0%/1.00% | Max/Avg Kernel=0.0%/0.00%
IPC-SUBSCRIBES: Prio=0 | Max/Avg User=1.0%/1.00% | Max/Avg Kernel=0.0%/0.00%
Core_poseThread: Prio=0 | Max/Avg User=1.0%/1.00% | Max/Avg Kernel=0.0%/0.00%