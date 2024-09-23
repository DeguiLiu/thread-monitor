# -*- coding: utf-8 -*-

import os
import struct
import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.dates as mdates
import os
import argparse
from unqlite import UnQLite

# 定义位域结构体
COMPACT_CPU_USAGE_FORMAT = (
    "<BBHHIHBBB"  # 小端序，B: unsigned char, H: unsigned short, I: unsigned int
)
COMPACT_CPU_USAGE_SIZE = struct.calcsize(COMPACT_CPU_USAGE_FORMAT)


# 检查文件是否存在且可读
def check_file_readable(filename):
    if not os.path.exists(filename):
        raise FileNotFoundError(f"File {filename} does not exist.")
    if not os.access(filename, os.R_OK):
        raise PermissionError(f"File {filename} is not readable.")


class UnqliteDB:
    def __init__(self, filename):
        self.db = UnQLite(filename)

    def fetch(self, key):
        try:
            return self.db[key]
        except KeyError:
            return None

    def close(self):
        self.db.close()


def load_thread_id_mapping(db):
    key = "thread_id_map"
    value = db.fetch(key)

    thread_id_map = {}
    if value:
        # 将字节对象解码为字符串
        value = value.decode("utf-8")
        for item in value.split(";"):
            if item:
                real_id, compressed_id = item.split(":")
                thread_id_map[int(real_id)] = int(compressed_id)
    return thread_id_map


def load_thread_name_mapping(db):
    key = "thread_name_map"
    value = db.fetch(key)

    thread_name_map = {}
    if value:
        value = value.decode("utf-8")
        for item in value.split(";"):
            if item:
                compressed_id, name = item.split(":", 1)
                thread_name_map[int(compressed_id)] = name
    return thread_name_map


def load_process_name(db):
    key = "process_name"
    value = db.fetch(key)

    if value:
        return value.decode("utf-8")
    else:
        return "Unknown Process"


COMPACT_CPU_USAGE_SIZE = 10  # 每个结构体的字节大小为10字节

def parse_compact_cpu_usage_data(raw_data):
    """
    解析原始的CPU使用数据，提取紧凑的CPU使用信息。

    参数：
    raw_data (bytes): 包含压缩CPU使用数据的字节串。

    返回：
    list: 一个包含解析后的CPU使用数据的字典列表，每个字典对应一个线程的使用数据。
    """
    records = []  # 用于存储解析后的记录

    # 遍历原始数据，以每10字节为一个数据块进行解析
    for i in range(0, len(raw_data), COMPACT_CPU_USAGE_SIZE):
        # 从原始数据中提取当前数据块
        packed_data = raw_data[i : i + COMPACT_CPU_USAGE_SIZE]

        # 检查提取的数据块是否完整
        if len(packed_data) != COMPACT_CPU_USAGE_SIZE:
            print(
                f"Error: Expected packed data size {COMPACT_CPU_USAGE_SIZE}, but got {len(packed_data)}."
            )
            continue  # 如果数据块大小不正确，跳过该数据块

        # 使用小端字节序将原始数据解包为一个整数 (full_data)
        full_data = int.from_bytes(packed_data, "little")

        # 按照位域定义，逐个提取各个字段的值
        user_percent = (full_data >> 0) & 0x7F  # 提取 user_percent 的 7 bits（用户模式下的CPU使用百分比）
        kernel_percent = (full_data >> 7) & 0x7F  # 提取 kernel_percent 的 7 bits（内核模式下的CPU使用百分比）
        user_ticks = (full_data >> 14) & 0xFFFF  # 提取 user_ticks 的 16 bits（用户模式下的CPU时间片数）
        kernel_ticks = (full_data >> 30) & 0xFFFF  # 提取 kernel_ticks 的 16 bits（内核模式下的CPU时间片数）
        timestamp = (full_data >> 46) & 0xFFFFF  # 提取 timestamp 的 20 bits（时间戳，表示自某个时刻的秒数）
        thread_id = (full_data >> 66) & 0x7F  # 调整位移，原代码可能有误
        thread_status = (full_data >> 73) & 0x03  # 调整位移
        extra_flags = (full_data >> 75) & 0x07  # 调整位移

        # 将解析后的各字段值存入字典，并添加到结果列表中
        records.append(
            {
                "user_percent": user_percent,  # 用户模式下的CPU使用百分比
                "kernel_percent": kernel_percent,  # 内核模式下的CPU使用百分比
                "user_ticks": user_ticks,  # 用户模式下的CPU时间片数
                "kernel_ticks": kernel_ticks,  # 内核模式下的CPU时间片数
                "timestamp": timestamp,  # 压缩后的时间戳
                "thread_id": thread_id,  # 压缩后的线程ID
                "thread_status": thread_status,  # 线程状态
                "extra_flags": extra_flags,  # 额外的标志位
            }
        )

    return records  # 返回所有解析后的记录列表


def read_db_data(db_filename):
    db = UnqliteDB(db_filename)
    thread_id_map = load_thread_id_mapping(db)
    thread_name_map = load_thread_name_mapping(db)
    process_name = load_process_name(db)  # 读取进程名称
    keys = db.db.keys()
    keys = sorted(db.db.keys())
    temp_data = {}

    for key in keys:
        if isinstance(key, bytes):
            key_str = key.decode("utf-8")
        else:
            key_str = key

        if key_str.startswith("batch_"):
            raw_data = db.db[key]
            parsed_records = parse_compact_cpu_usage_data(raw_data)

            for record in parsed_records:
                compressed_thread_id = record["thread_id"]
                real_thread_id = thread_id_map.get(compressed_thread_id, "unknown")
                thread_name = thread_name_map.get(compressed_thread_id, f"tid_{real_thread_id}")

                thread_key = thread_name  # 使用线程名称而不是通用ID

                if thread_key not in temp_data:
                    temp_data[thread_key] = {
                        "timestamp": [record["timestamp"]],
                        "thread_name": thread_key,
                        "user_usage": [record["user_percent"]],
                        "kernel_usage": [record["kernel_percent"]],
                    }
                else:
                    temp_data[thread_key]["timestamp"].append(record["timestamp"])
                    temp_data[thread_key]["user_usage"].append(record["user_percent"])
                    temp_data[thread_key]["kernel_usage"].append(record["kernel_percent"])

    db.close()

    # 转换为 DataFrame
    frames = []
    for thread_name, usage in temp_data.items():
        df = pd.DataFrame(
            {
                "timestamp": pd.to_datetime(usage["timestamp"], unit="s"),
                "thread_name": thread_name,
                "user_usage": usage["user_usage"],
                "kernel_usage": usage["kernel_usage"],
            }
        )
        frames.append(df)

    if frames:
        data = pd.concat(frames, ignore_index=True)
    else:
        data = pd.DataFrame()

    return data, process_name  # 返回数据和进程名称


# 计算每个线程的统计数据（最小值、最大值、平均值）
def calculate_statistics(subset):
    stats = {}

    if "user_usage" in subset.columns:
        stats["min_user"] = subset["user_usage"].min()
        stats["max_user"] = subset["user_usage"].max()
        stats["mean_user"] = subset["user_usage"].mean()

    if "kernel_usage" in subset.columns:
        stats["min_kernel"] = subset["kernel_usage"].min()
        stats["max_kernel"] = subset["kernel_usage"].max()
        stats["mean_kernel"] = subset["kernel_usage"].mean()

    return stats


# 计算并返回进程的总CPU使用情况
def calculate_process_cpu(data):
    process_cpu = (
        data.groupby("timestamp")
        .agg({"user_usage": "sum", "kernel_usage": "sum"})
        .reset_index()
    )

    # 过滤掉总和为0的行
    process_cpu = process_cpu[
        (process_cpu["user_usage"] > 0) | (process_cpu["kernel_usage"] > 0)
    ]

    process_cpu["total_usage"] = process_cpu["user_usage"] + process_cpu["kernel_usage"]
    return process_cpu


# 打印进程和线程的基本信息和统计数据
def get_summary_table(thread_info, data, process_name="Unknown Process"):
    summary_lines = []
    summary_lines.append(f"Process Name: {process_name}")

    for thread_name in data["thread_name"].unique():
        subset = data[data["thread_name"] == thread_name]

        user_exists = "user_usage" in subset.columns
        kernel_exists = "kernel_usage" in subset.columns

        if (
            user_exists
            and subset["user_usage"].sum() == 0
            and kernel_exists
            and subset["kernel_usage"].sum() == 0
        ):
            continue

        stats = calculate_statistics(subset)

        user_stats = ""
        kernel_stats = ""
        if user_exists:
            user_stats = f"Max/Avg User={stats['max_user']}%/{stats['mean_user']:.2f}%"
        if kernel_exists:
            kernel_stats = (
                f"Max/Avg Kernel={stats['max_kernel']}%/{stats['mean_kernel']:.2f}%"
            )

        summary_lines.append(f"{thread_name}: {user_stats} | {kernel_stats}")
    return "\n".join(summary_lines)


def plot_cpu_usage(
    thread_info,
    data,
    process_name="Unknown Process",  # 添加进程名称参数
    filter_thread=None,
    filter_cpu_type=None,
    time_range=None,
    show_summary_info=True,
):
    plt.figure(figsize=(14, 10))

    # 计算进程总CPU使用情况
    process_cpu = calculate_process_cpu(data)

    # 将时间戳转换为可读的时间格式
    process_cpu["timestamp"] = pd.to_datetime(process_cpu["timestamp"], unit="s")
    process_cpu = process_cpu.sort_values("timestamp")

    # 绘制进程总CPU使用情况曲线
    plt.plot(
        process_cpu["timestamp"],
        process_cpu["total_usage"],
        label="Process Total CPU Usage",
        color="black",
        linewidth=2,
    )

    # 过滤线程或CPU类型
    if filter_thread:
        data = data[data["thread_name"].str.contains(filter_thread, case=False)]
    if filter_cpu_type:
        if filter_cpu_type.lower() == "user":
            data = data[["timestamp", "thread_name", "user_usage"]]
        elif filter_cpu_type.lower() == "kernel":
            data = data[["timestamp", "thread_name", "kernel_usage"]]

    if time_range:
        start_time, end_time = time_range
        data = data[(data["timestamp"] >= start_time) & (data["timestamp"] <= end_time)]

    data["timestamp"] = pd.to_datetime(data["timestamp"])

    # 绘制每个线程的CPU使用情况曲线
    for thread_name in data["thread_name"].unique():
        subset = data[data["thread_name"] == thread_name]
        subset = subset.sort_values("timestamp")

        user_sum = subset.get("user_usage", pd.Series([0]))
        kernel_sum = subset.get("kernel_usage", pd.Series([0]))

        if user_sum.sum() + kernel_sum.sum() == 0:
            continue

        subset = subset.fillna(0)

        # 绘制用户模式的CPU使用曲线
        if "user_usage" in subset.columns:
            plt.plot(
                subset["timestamp"],
                subset["user_usage"],
                label=f"{thread_name} (User)",
                linestyle="--",
            )

        # 绘制内核模式的CPU使用曲线
        if "kernel_usage" in subset.columns:
            plt.plot(
                subset["timestamp"],
                subset["kernel_usage"],
                label=f"{thread_name} (Kernel)",
                linestyle=":",
            )

    plt.xlabel("Time")
    plt.ylabel("CPU Usage (%)")
    plt.title(f"CPU Usage Over Time by Thread for Process: {process_name}")  # 在标题中显示进程名称
    plt.gcf().autofmt_xdate()  # 自动格式化日期标签
    plt.legend(loc="upper left", bbox_to_anchor=(1, 1))
    plt.grid(True)
    plt.tight_layout(rect=[0, 0.1, 1, 0.95])

    if show_summary_info:
        summary_info = get_summary_table(thread_info, data, process_name)

        plt.figtext(
            0.02,
            0.01,
            summary_info,
            fontsize=9,
            verticalalignment="bottom",
            horizontalalignment="left",
            bbox=dict(facecolor="white", alpha=0.5),
        )

    plt.savefig("cpu_usage_over_time.png")
    plt.show()


def main():
    parser = argparse.ArgumentParser(
        description="Analyze and plot CPU usage data from a UnQLite database file."
    )
    parser.add_argument(
        "filename", type=str, help="The path to the UnQLite database file."
    )
    parser.add_argument(
        "--filter-thread", type=str, help="Filter by thread name (case insensitive)."
    )
    parser.add_argument(
        "--filter-cpu-type",
        type=str,
        choices=["user", "kernel"],
        help="Filter by CPU usage type ('user' or 'kernel').",
    )
    parser.add_argument(
        "--time-range",
        type=str,
        help="Filter by time range, format: 'start_time,end_time' (e.g., '12:00:00,12:30:00').",
    )
    parser.add_argument(
        "--hide-summary",
        action="store_true",
        help="Hide the process and thread summary information at the bottom of the plot.",
    )

    args = parser.parse_args()

    try:
        # 读取数据和进程名称
        data, process_name = read_db_data(args.filename)

        if data.empty:
            print("No data found in the database.")
            return

        # 打印基本信息和统计数据
        if not args.hide_summary:
            print(get_summary_table({}, data, process_name))

        time_range = None
        if args.time_range:
            start_time, end_time = args.time_range.split(",")
            time_range = (start_time, end_time)

        plot_cpu_usage(
            {},
            data,
            process_name=process_name,  # 传递进程名称
            filter_thread=args.filter_thread,
            filter_cpu_type=args.filter_cpu_type,
            time_range=time_range,
            show_summary_info=not args.hide_summary,
        )

    except Exception as e:
        print(f"An error occurred: {e}")


if __name__ == "__main__":
    main()
