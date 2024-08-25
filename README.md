## 概述

本工具是一个结合 C++ 和 Python 的 CPU 使用率监控与分析工具。通过读取 Linux 系统的 `/proc` 文件系统，实时采集 CPU 使用数据，使用 UnQLite 数据库进行高效存储，并通过 Python 进行数据解析和可视化展示。该工具支持对多个进程的 CPU 使用情况进行同时监控和分析。

## 编译与运行

### 使用 UnQLite 数据库存储方案

1. **启动模拟进程 (`dummp_worker`)**

   `dummp_worker` 是一个用于模拟的待分析进程：
   ```bash
   sudo ./dummp_worker 2 0.4
   ```
   该命令将启动两个线程，每个线程占用 40% 的 CPU。

2. **编译并运行 C++ 程序**

   使用以下命令编译 C++ 程序：
   ```bash
   g++ -o thread_cpu_unqlite thread_cpu_unqlite.cc -lunqlite -O2
   ```

   运行 C++ 程序进行数据采集：
   ```bash
   ./thread_cpu_unqlite -n1 dummp_worker -d dummp_worker.db
   ```
   该命令将每秒采集一次 `dummp_worker` 进程的 CPU 使用数据，并将结果存储在 `dummp_worker.db` 中。

3. **使用 Python 进行数据解析和可视化**

   安装所需的 Python 库：
   ```bash
   pip3 install unqlite pandas matplotlib
   ```

   运行 Python 脚本解析和可视化数据：
   ```bash
   python3 thread_cpu_unqlite4.py dummp_worker.db
   ```

### 使用 CSV 文件存储方案

1. **启动模拟进程 (`dummp_worker`)**

   与 UnQLite 方案相同，启动模拟进程：
   ```bash
   sudo ./dummp_worker 2 0.4
   ```

2. **编译并运行 C++ 程序**

   使用以下命令编译 C++ 程序：
   ```bash
   g++ -o thread_cpu_get thread_cpu_get.cc -O2
   ```

   运行 C++ 程序进行数据采集，并将结果保存为 CSV 文件：
   ```bash
   ./thread_cpu_get -n1 dummp_worker
   ```

3. **使用 Python 解析 CSV 文件**

   安装所需的 Python 库：
   ```bash
   pip3 install unqlite pandas matplotlib
   ```

   运行 Python 脚本解析 CSV 文件并进行数据分析：
   ```bash
   python3 thread_cpu_parse.py process_dummp_worker.csv
   ```

