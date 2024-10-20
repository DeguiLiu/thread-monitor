# -*- coding: utf-8 -*-

import os
import struct
import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.font_manager as font_manager
import argparse
from datetime import datetime
import sys

# Update the size of each CpuUsageData record (in bytes)
CPU_USAGE_SIZE = 22  # Updated to 22 bytes

def parse_cpu_usage_data(record_bytes):
    """
    Parse a single CpuUsageData record from bytes.

    Parameters:
    record_bytes (bytes): 22-byte binary data representing CPU usage.

    Returns:
    dict: Parsed fields from the record.
    """
    if len(record_bytes) != CPU_USAGE_SIZE:
        raise ValueError("Record size must be 22 bytes")

    # Define the struct format: little-endian
    # H: uint16_t user_percent
    # H: uint16_t kernel_percent
    # I: uint32_t user_ticks
    # I: uint32_t kernel_ticks
    # I: uint32_t timestamp
    # I: uint32_t thread_id
    # B: uint8_t thread_status
    # B: uint8_t extra_flags
    struct_format = '<HHIIIIBB'
    unpacked_data = struct.unpack(struct_format, record_bytes)

    record = {
        "user_percent": unpacked_data[0],
        "kernel_percent": unpacked_data[1],
        "user_ticks": unpacked_data[2],
        "kernel_ticks": unpacked_data[3],
        "timestamp": unpacked_data[4],
        "thread_id": unpacked_data[5],
        "thread_status": unpacked_data[6],
        "extra_flags": unpacked_data[7],
    }

    return record

def read_file_header(f):
    """
    Read and parse the file header from cpu_usage.bin.

    Parameters:
    f (file object): Opened binary file object positioned at the beginning.

    Returns:
    tuple: (process_name (str), thread_name_map (dict))
    """
    # Read header_size (4 bytes)
    header_size_data = f.read(4)
    if len(header_size_data) < 4:
        raise ValueError("Failed to read header size.")
    header_size = struct.unpack("<I", header_size_data)[0]

    # Read the rest of the header
    header_data = f.read(header_size)
    if len(header_data) < header_size:
        raise ValueError("Failed to read complete header.")

    offset = 0

    # Read process_name_length (4 bytes)
    process_name_length = struct.unpack_from("<I", header_data, offset)[0]
    offset += 4

    # Read process_name
    process_name = header_data[offset : offset + process_name_length].decode("utf-8")
    offset += process_name_length

    # Read thread_map_size (4 bytes)
    thread_map_size = struct.unpack_from("<I", header_data, offset)[0]
    offset += 4

    thread_name_map = {}
    for _ in range(thread_map_size):
        # Read thread_id (4 bytes)
        thread_id = struct.unpack_from("<I", header_data, offset)[0]
        offset += 4

        # Read thread_name_length (4 bytes)
        thread_name_length = struct.unpack_from("<I", header_data, offset)[0]
        offset += 4

        # Read thread_name
        thread_name = header_data[offset : offset + thread_name_length].decode("utf-8")
        offset += thread_name_length

        thread_name_map[thread_id] = thread_name

    return process_name, thread_name_map

def read_cpu_usage_bin(filename):
    """
    Read and parse the cpu_usage.bin file.

    Parameters:
    filename (str): Path to the cpu_usage.bin file.

    Returns:
    tuple: (records (list of dict), process_name (str), thread_name_map (dict))
    """
    records = []
    process_name = "Unknown Process"
    thread_name_map = {}

    try:
        with open(filename, "rb") as f:
            # Read and parse the header
            process_name, thread_name_map = read_file_header(f)

            # Read and parse each CpuUsageData record
            while True:
                record_bytes = f.read(CPU_USAGE_SIZE)
                if not record_bytes or len(record_bytes) < CPU_USAGE_SIZE:
                    break
                record = parse_cpu_usage_data(record_bytes)
                records.append(record)

    except FileNotFoundError:
        print(f"File {filename} not found.")
    except Exception as e:
        print(f"Error reading binary file: {e}")

    return records, process_name, thread_name_map

def parse_records_to_dataframe(records, thread_name_map):
    """
    Convert parsed records to a pandas DataFrame.

    Parameters:
    records (list of dict): Parsed CPU usage records.
    thread_name_map (dict): Mapping from thread_id to thread_name.

    Returns:
    pandas.DataFrame: DataFrame containing the CPU usage data.
    """
    data = pd.DataFrame(records)
    if data.empty:
        return data  # Return empty DataFrame if no records
    # Ensure thread_id is integer
    data["thread_id"] = data["thread_id"].astype(int)
    # Map thread_id to thread_name
    data["thread_name"] = data["thread_id"].map(thread_name_map).fillna("unknown")
    # Convert timestamp to datetime (assuming timestamp is seconds since epoch)
    data["timestamp"] = pd.to_datetime(data["timestamp"], unit="s")

    # Adjust percentage values: Convert user_percent and kernel_percent from 0-10000 to 0-100%
    data["user_percent"] = data["user_percent"] / 100.0
    data["kernel_percent"] = data["kernel_percent"] / 100.0

    return data

def calculate_statistics(subset):
    """
    Calculate statistics (min, max, mean) for total CPU usage.

    Parameters:
    subset (pandas.DataFrame): Subset of data for a specific thread.

    Returns:
    dict: Calculated statistics.
    """
    stats = {}

    if "total_usage" in subset.columns:
        stats["min_total"] = subset["total_usage"].min()
        stats["max_total"] = subset["total_usage"].max()
        stats["mean_total"] = subset["total_usage"].mean()

    return stats

def calculate_process_cpu(data):
    """
    Calculate the total CPU usage of the process over time.

    Parameters:
    data (pandas.DataFrame): DataFrame containing CPU usage data.

    Returns:
    pandas.DataFrame: DataFrame with total CPU usage per timestamp.
    """
    process_cpu = (
        data.groupby("timestamp")
        .agg({"user_percent": "sum", "kernel_percent": "sum"})
        .reset_index()
    )

    # Filter out rows where total usage is 0
    process_cpu = process_cpu[
        (process_cpu["user_percent"] > 0) | (process_cpu["kernel_percent"] > 0)
    ]

    process_cpu["total_usage"] = (
        process_cpu["user_percent"] + process_cpu["kernel_percent"]
    )
    return process_cpu

def get_summary_table(data, process_name="Unknown Process"):
    """
    Generate a summary table of CPU usage statistics for each thread.

    Parameters:
    data (pandas.DataFrame): DataFrame containing CPU usage data.
    process_name (str): Name of the process.

    Returns:
    str: Summary table as a string.
    """
    if data.empty:
        return f"Process Name: {process_name}\nNo data available."

    # Ensure 'total_usage' is calculated
    if "total_usage" not in data.columns:
        data["total_usage"] = data["user_percent"] + data["kernel_percent"]

    # Calculate average and max CPU usage per thread and sort
    thread_stats = data.groupby("thread_name")["total_usage"].agg(['mean', 'max']).reset_index()
    # Sort by average CPU usage in descending order
    thread_stats = thread_stats.sort_values(by='mean', ascending=False)

    # Prepare summary table header
    header = f"{'thread_name':30s} | {'Max CPU%':>8s} | {'Avg CPU%':>8s}"
    separator = "-" * len(header)
    summary_lines = [f"Process Name: {process_name}", separator, header, separator]

    # Format statistics for each thread
    for _, row in thread_stats.iterrows():
        thread_name = row['thread_name']
        max_cpu = f"{row['max']:.2f}"
        avg_cpu = f"{row['mean']:.2f}"
        line = f"{thread_name:30s} | {max_cpu:>8s} | {avg_cpu:>8s}"
        summary_lines.append(line)

    # Add separator at the end
    summary_lines.append(separator)

    return "\n".join(summary_lines)

def split_summary_table(summary_lines):
    """
    Split the summary table into two columns with headers.

    Parameters:
    summary_lines (list): List of lines in the summary table.

    Returns:
    tuple: (left_column (str), right_column (str))
    """
    # Find the indices of the header lines
    separator_indices = [i for i, line in enumerate(summary_lines) if set(line) == {"-"}]
    if len(separator_indices) < 2:
        # Not enough separators, return as is
        return ("\n".join(summary_lines), "")

    # Extract the header and separator
    header = summary_lines[separator_indices[0]+1]
    separator_line = summary_lines[separator_indices[0]]

    # Split the data lines
    data_lines = summary_lines[separator_indices[1]+1:-1]  # Exclude last separator
    half = (len(data_lines) + 1) // 2  # Ensure the first half is at least as big as the second

    # Build left and right columns
    left_lines = [separator_line, header, separator_line] + data_lines[:half] + [separator_line]
    right_lines = [separator_line, header, separator_line] + data_lines[half:] + [separator_line]

    left_column = "\n".join(left_lines)
    right_column = "\n".join(right_lines)

    return (left_column, right_column)

def plot_cpu_usage(
    data,
    process_name="Unknown Process",
    filter_thread=None,
    filter_cpu_type=None,
    time_range=None,
    show_summary_info=True,
    top_n=10,
    separate_cpu=False,
    output_filename="cpu_usage_over_time.png",
):
    """
    Plot CPU usage over time for the process and its threads.

    Parameters:
    data (pandas.DataFrame): DataFrame containing CPU usage data.
    process_name (str): Name of the process.
    filter_thread (str, optional): Filter to include only specific thread_names.
    filter_cpu_type (str, optional): Filter to include only 'user' or 'kernel' CPU usage.
    time_range (tuple, optional): Tuple of (start_time, end_time) to filter the data.
    show_summary_info (bool): Whether to display summary information at the bottom of the plot.
    top_n (int): Number of top threads to display based on average CPU usage.
    separate_cpu (bool): Whether to plot user and kernel CPU usages separately.
    output_filename (str): Filename to save the plot.
    """
    if data.empty:
        print("No data to plot.")
        return

    plt.figure(figsize=(14, 10))

    # Ensure 'total_usage' is calculated
    data["total_usage"] = data["user_percent"] + data["kernel_percent"]

    # Calculate total CPU usage of the process
    process_cpu = calculate_process_cpu(data)

    # Sort by timestamp
    process_cpu = process_cpu.sort_values("timestamp")

    # Plot total CPU usage of the process
    plt.plot(
        process_cpu["timestamp"],
        process_cpu["total_usage"],
        label="Process Total CPU Usage",
        color="black",
        linewidth=2,
    )

    # Apply filters if any
    if filter_thread:
        data = data[data["thread_name"].str.contains(filter_thread, case=False)]

    if time_range:
        start_time, end_time = time_range
        data = data[(data["timestamp"] >= start_time) & (data["timestamp"] <= end_time)]

    # Calculate average CPU usage per thread and select top N threads
    avg_cpu_usage = data.groupby("thread_name")["total_usage"].mean()
    top_threads = avg_cpu_usage.nlargest(top_n).index.tolist()
    data = data[data["thread_name"].isin(top_threads)]

    # Sort threads by average CPU usage
    sorted_threads = avg_cpu_usage.loc[top_threads].sort_values(ascending=False).index.tolist()

    # Sort data by timestamp and thread CPU usage
    data = data.reset_index()
    data['thread_name'] = pd.Categorical(data['thread_name'], categories=sorted_threads, ordered=True)
    data = data.sort_values(["thread_name", "timestamp"])

    # Set timestamp as index for resampling
    data.set_index("timestamp", inplace=True)

    # Resampling frequency
    resample_freq = 'S'  # 1 second

    # Plot CPU usage for each thread, in order of CPU usage
    lines = []
    labels = []

    for thread_name in sorted_threads:
        subset = data[data["thread_name"] == thread_name]

        if subset.empty:
            continue

        # Resample to ensure continuity
        if separate_cpu:
            # Plot user and kernel CPU usage separately
            user_usage = subset["user_percent"].resample(resample_freq).mean().interpolate()
            kernel_usage = subset["kernel_percent"].resample(resample_freq).mean().interpolate()

            line_user, = plt.plot(
                user_usage.index,
                user_usage.values,
                label=f"{thread_name} (User)",
                linestyle="-",
            )
            line_kernel, = plt.plot(
                kernel_usage.index,
                kernel_usage.values,
                label=f"{thread_name} (Kernel)",
                linestyle="--",
            )
            lines.extend([line_user, line_kernel])
            labels.extend([f"{thread_name} (User)", f"{thread_name} (Kernel)"])
        else:
            # Plot combined CPU usage
            total_usage = subset["total_usage"].resample(resample_freq).mean().interpolate()
            line, = plt.plot(
                total_usage.index,
                total_usage.values,
                label=f"{thread_name}",
                linestyle="-",
            )
            lines.append(line)
            labels.append(f"{thread_name}")

    plt.xlabel("Time")
    plt.ylabel("CPU Usage (%)")
    plt.title(f"CPU Usage Over Time by Thread for Process: {process_name}")
    plt.gcf().autofmt_xdate()

    # Create legend with sorted entries
    plt.legend(lines, labels, loc="upper left", bbox_to_anchor=(1, 1))

    plt.grid(True)

    # Adjust layout based on whether summary info is shown
    if show_summary_info:
        plt.tight_layout(rect=[0, 0.20, 1, 0.95])
    else:
        plt.tight_layout(rect=[0, 0.02, 1, 0.95])

    if show_summary_info:
        summary_info = get_summary_table(data.reset_index(), process_name)

        # Split summary information into two columns with headers
        summary_lines = summary_info.split('\n')
        left_column, right_column = split_summary_table(summary_lines)

        # Use monospace font for alignment
        monospace_font = font_manager.FontProperties(family='monospace', size=9)

        # Display two columns of summary information at the bottom of the plot
        plt.figtext(
            0.02,
            0.01,
            left_column,
            fontsize=9,
            fontproperties=monospace_font,
            verticalalignment="bottom",
            horizontalalignment="left",
            bbox=dict(facecolor="white", alpha=0.5),
        )
        if right_column.strip():
            plt.figtext(
                0.32,  # Adjust the position of the right column
                0.01,
                right_column,
                fontsize=9,
                fontproperties=monospace_font,
                verticalalignment="bottom",
                horizontalalignment="left",
                bbox=dict(facecolor="white", alpha=0.5),
            )

    try:
        plt.savefig(output_filename)
        plt.show()
    except KeyboardInterrupt:
        print("\nPlotting interrupted by user. Exiting gracefully.")
        plt.close()
        sys.exit(0)

def main():
    """
    Main function to parse arguments, read data, and generate plots.
    """
    parser = argparse.ArgumentParser(
        description="Analyze and plot CPU usage data from a binary file."
    )
    parser.add_argument(
        "filename", type=str, help="The path to the cpu_usage.bin file."
    )
    parser.add_argument("--filter-thread", type=str, help="Filter by thread_name.")
    parser.add_argument(
        "--filter-cpu-type",
        type=str,
        choices=["user", "kernel"],
        help="Filter by CPU usage type ('user' or 'kernel').",
    )
    parser.add_argument(
        "--time-range",
        type=str,
        help="Filter by time range, format: 'start_time,end_time' (e.g., '2024-09-24 12:00:00,2024-09-24 12:30:00').",
    )
    parser.add_argument(
        "--hide-summary",
        action="store_true",
        help="Hide the process and thread summary information at the bottom of the plot.",
    )
    parser.add_argument(
        "--top",
        type=int,
        default=10,
        help="Number of top threads to display based on average CPU usage.",
    )
    parser.add_argument(
        "--separate-cpu",
        action="store_true",
        help="Plot user and kernel CPU usages separately.",
    )
    parser.add_argument(
        "--output-file",
        type=str,
        default="cpu_usage_over_time.png",
        help="Filename to save the output plot.",
    )

    args = parser.parse_args()

    # Add the following print statements to display parameters
    print("Parsed command-line arguments:")
    print(f"Input filename       : {args.filename}")
    print(f"Filter thread        : {args.filter_thread}")
    print(f"Filter CPU type      : {args.filter_cpu_type}")
    print(f"Time range           : {args.time_range}")
    print(f"Hide summary         : {args.hide_summary}")
    print(f"Top N threads        : {args.top}")
    print(f"Separate CPU         : {args.separate_cpu}")
    print(f"Output file          : {args.output_file}")

    try:
        # Read and parse the binary file
        records, process_name, thread_name_map = read_cpu_usage_bin(args.filename)
        if not records:
            print("No data found in the binary file.")
            return

        # Convert records to DataFrame
        data = parse_records_to_dataframe(records, thread_name_map)
        if data.empty:
            print("No data available to process.")
            return

        # Ensure 'total_usage' is calculated
        data["total_usage"] = data["user_percent"] + data["kernel_percent"]

        # Print summary information
        if not args.hide_summary:
            print(get_summary_table(data, process_name))

        # Handle time range filtering
        time_range = None
        if args.time_range:
            try:
                start_str, end_str = args.time_range.split(",")
                start_time = pd.to_datetime(start_str.strip())
                end_time = pd.to_datetime(end_str.strip())
                time_range = (start_time, end_time)
            except Exception as e:
                print(f"Invalid time range format: {e}")
                return

        # Plot CPU usage
        plot_cpu_usage(
            data,
            process_name=process_name,
            filter_thread=args.filter_thread,
            filter_cpu_type=args.filter_cpu_type,
            time_range=time_range,
            show_summary_info=not args.hide_summary,
            top_n=args.top,
            separate_cpu=args.separate_cpu,
            output_filename=args.output_file,
        )

    except KeyboardInterrupt:
        print("\nExecution interrupted by user. Exiting gracefully.")
        sys.exit(0)
    except Exception as e:
        print(f"An error occurred: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
