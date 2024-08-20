import pandas as pd
import matplotlib.pyplot as plt
import os
import sys
import argparse

# 检查文件是否存在且可读
def check_file_readable(filename):
    if not os.path.exists(filename):
        raise FileNotFoundError(f"File {filename} does not exist。")
    if not os.access(filename, os.R_OK):
        raise PermissionError(f"File {filename} is not readable。")

# 解析文件头部信息，获取进程和线程信息
def parse_header_info(filename):
    process_info = {}
    thread_info = {}
    data_start_line = 0
    line_number = 0
    with open(filename, 'r') as file:
        lines = file.readlines()
        for line in lines:
            line_number += 1
            try:
                if line.startswith("Process ID:"):
                    process_info['id'] = line.strip().split(": ")[1]
                elif line.startswith("Process Priority:"):
                    process_info['priority'] = line.strip().split(": ")[1]
                elif line.startswith("Thread ID:"):
                    parts = line.strip().split(',')
                    thread_id = parts[0].split(': ')[1]
                    thread_name = parts[1].split(': ')[1]
                    thread_priority = parts[2].split(': ')[1]
                    thread_info[thread_name] = {
                        'id': thread_id,
                        'priority': thread_priority
                    }
                elif line.startswith("Time,Thread Name,Thread ID,User %,Kernel %"):
                    data_start_line = line_number + 1
                    break
            except Exception as e:
                print(f"Error parsing line {line_number}: {line.strip()}")
                print(f"Error details: {e}")
    return process_info, thread_info, data_start_line

# 逐行读取数据，捕获并打印错误行号，并聚合相同线程名称的CPU使用量
def read_data(filename, start_line):
    check_file_readable(filename)
    data = []
    line_number = 0
    temp_data = {}  # 用于聚合相同线程名称的CPU使用量

    with open(filename, 'r') as file:
        for line in file:
            line_number += 1
            if line_number >= start_line:  # 从数据开始行读取数据
                try:
                    timestamp, thread_name, thread_id, user_usage, kernel_usage = line.strip().split(',')
                    user_usage = float(user_usage)
                    kernel_usage = float(kernel_usage)

                    # 如果 User 和 Kernel CPU 使用率都为 0，则不保存此行数据
                    if user_usage == 0 and kernel_usage == 0:
                        continue

                    if thread_name not in temp_data:
                        temp_data[thread_name] = {
                            'timestamp': [timestamp],
                            'user_usage': [user_usage],
                            'kernel_usage': [kernel_usage]
                        }
                    else:
                        temp_data[thread_name]['timestamp'].append(timestamp)
                        temp_data[thread_name]['user_usage'].append(user_usage)
                        temp_data[thread_name]['kernel_usage'].append(kernel_usage)

                except ValueError as e:
                    print(f"Error parsing line {line_number}: {line.strip()}")
                    print(f"Error details: {e}")

    # 将聚合后的数据转换为 DataFrame
    frames = []
    for thread_name, usage in temp_data.items():
        df = pd.DataFrame({
            'timestamp': usage['timestamp'],
            'thread_name': thread_name,
            'user_usage': usage['user_usage'],
            'kernel_usage': usage['kernel_usage']
        })
        frames.append(df)

    return pd.concat(frames, ignore_index=True)

# 计算每个线程的统计数据（最小值、最大值、平均值）
def calculate_statistics(subset):
    stats = {
        'min_user': subset['user_usage'].min(),
        'max_user': subset['user_usage'].max(),
        'mean_user': subset['user_usage'].mean(),
        'min_kernel': subset['kernel_usage'].min(),
        'max_kernel': subset['kernel_usage'].max(),
        'mean_kernel': subset['kernel_usage'].mean()
    }
    return stats

# 计算并返回进程的总CPU使用情况
def calculate_process_cpu(data):
    process_cpu = data.groupby('timestamp').agg({
        'user_usage': 'sum',
        'kernel_usage': 'sum'
    }).reset_index()

    process_cpu['total_usage'] = process_cpu['user_usage'] + process_cpu['kernel_usage']
    return process_cpu

# 打印进程和线程的基本信息和统计数据
def get_summary_table(process_info, thread_info, data):
    summary_lines = []
    summary_lines.append(f"Process ID: {process_info['id']} | Prio: {process_info['priority']}")

    for thread_name in data['thread_name'].unique():
        subset = data[data['thread_name'] == thread_name]

        # 如果线程的所有数据的 User 和 Kernel CPU 都为 0，则跳过
        if subset['user_usage'].sum() == 0 and subset['kernel_usage'].sum() == 0:
            continue

        # 计算统计数据
        stats = calculate_statistics(subset)
        thread_info_subset = thread_info.get(thread_name, {})
        thread_priority = thread_info_subset.get('priority', 'Unknown')
        summary_lines.append(f"{thread_name}: Prio={thread_priority} | Max/Avg User={stats['max_user']}%/{stats['mean_user']:.2f}% | Max/Avg Kernel={stats['max_kernel']}%/{stats['mean_kernel']:.2f}%")

    return "\n".join(summary_lines)

# 绘制每个线程的用户时间和内核时间占用百分比，并绘制整个进程的总CPU使用情况
def plot_cpu_usage(process_info, thread_info, data, filter_thread=None, filter_cpu_type=None, time_range=None, show_summary_info=True):
    plt.figure(figsize=(14, 10))  # 增加图表高度以留出更多空间放置文本

    # 计算进程总CPU使用情况
    process_cpu = calculate_process_cpu(data)

    # 绘制进程总CPU使用情况曲线
    plt.plot(process_cpu['timestamp'], process_cpu['total_usage'], label='Process Total CPU Usage', color='black', linewidth=2)

    # 过滤线程或CPU类型
    if filter_thread:
        data = data[data['thread_name'].str.contains(filter_thread, case=False)]
    if filter_cpu_type:
        if filter_cpu_type.lower() == 'user':
            data = data[['timestamp', 'thread_name', 'user_usage']]
        elif filter_cpu_type.lower() == 'kernel':
            data = data[['timestamp', 'thread_name', 'kernel_usage']]

    # 按时间范围过滤
    if time_range:
        start_time, end_time = time_range
        data = data[(data['timestamp'] >= start_time) & (data['timestamp'] <= end_time)]

    if show_summary_info:
        summary_info = get_summary_table(process_info, thread_info, data)

    for thread_name in data['thread_name'].unique():
        subset = data[data['thread_name'] == thread_name]

        # 如果线程的所有数据的 User 和 Kernel CPU 都为 0，则跳过
        if subset['user_usage'].sum() == 0 and subset['kernel_usage'].sum() == 0:
            continue

        # 处理异常数据，例如填充缺失值
        subset = subset.fillna(0)

        # 绘制线程的CPU使用曲线
        plt.plot(subset['timestamp'], subset['user_usage'], label=f'{thread_name} (User)', linestyle='--')
        plt.plot(subset['timestamp'], subset['kernel_usage'], label=f'{thread_name} (Kernel)', linestyle=':')

    plt.xlabel('Time (HH:MM:SS)')
    plt.ylabel('CPU Usage (%)')
    plt.title('CPU Usage Over Time by Thread')

    # 调整X轴标签的密度，显示一部分的标签
    x_ticks = plt.gca().get_xticks()
    plt.xticks(x_ticks[::max(1, len(x_ticks)//10)], rotation=45)  # 只显示部分标签

    plt.legend(loc='upper left', bbox_to_anchor=(1, 1))
    plt.grid(True)
    plt.tight_layout(rect=[0, 0.1, 1, 0.95])  # 调整布局，给底部留出空间放置文本

    # 在图的X轴下方显示简洁的进程和线程信息（如果配置为显示）
    if show_summary_info:
        plt.figtext(0.02, 0.01, summary_info, fontsize=9, verticalalignment='bottom', horizontalalignment='left', bbox=dict(facecolor='white', alpha=0.5))

    # 保存图表为 PNG 文件
    plt.savefig('cpu_usage_over_time_filtered.png')

    # 显示图表
    plt.show()

def main():
    # 创建命令行参数解析器
    parser = argparse.ArgumentParser(description="Analyze and plot CPU usage data from a CSV file.")
    parser.add_argument('filename', type=str, help="The path to the CSV file.")
    parser.add_argument('--filter-thread', type=str, help="Filter by thread name (case insensitive).")
    parser.add_argument('--filter-cpu-type', type=str, choices=['user', 'kernel'], help="Filter by CPU usage type ('user' or 'kernel').")
    parser.add_argument('--time-range', type=str, help="Filter by time range, format: 'start_time,end_time' (e.g., '12:00:00,12:30:00').")
    parser.add_argument('--hide-summary', action='store_true', help="Hide the process and thread summary information at the bottom of the plot.")

    args = parser.parse_args()

    try:
        # 解析头部信息
        process_info, thread_info, start_line = parse_header_info(args.filename)
        # 读取数据
        data = read_data(args.filename, start_line)

        # 打印基本信息和统计数据
        if not args.hide_summary:
            print(get_summary_table(process_info, thread_info, data))

        # 解析时间范围参数
        time_range = None
        if args.time_range:
            start_time, end_time = args.time_range.split(',')
            time_range = (start_time, end_time)

        # 调用函数并应用过滤器
        plot_cpu_usage(process_info, thread_info, data,
                       filter_thread=args.filter_thread,
                       filter_cpu_type=args.filter_cpu_type,
                       time_range=time_range,
                       show_summary_info=not args.hide_summary)

    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    main()
