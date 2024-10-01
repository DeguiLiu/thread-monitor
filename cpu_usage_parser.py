# -*- coding: utf-8 -*-

import os
import struct
import pandas as pd
import matplotlib.pyplot as plt
import argparse
from datetime import datetime
import sys

# Define the size of each CpuUsageData record in bytes
CPU_USAGE_SIZE = 16

def parse_cpu_usage_data(record_bytes):
    """
    Parse a single CpuUsageData record from bytes.

    Parameters:
    record_bytes (bytes): 16-byte binary data representing CPU usage.

    Returns:
    dict: Parsed fields from the record.
    """
    if len(record_bytes) != CPU_USAGE_SIZE:
        raise ValueError("Record size must be 16 bytes")

    # Define the struct format: little-endian
    # B: uint8_t user_percent
    # B: uint8_t kernel_percent
    # H: uint16_t user_ticks
    # H: uint16_t kernel_ticks
    # I: uint32_t timestamp
    # I: uint32_t thread_id
    # B: uint8_t thread_status
    # B: uint8_t extra_flags
    struct_format = '<BBHHIIBB'
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

    # Debug: Print the header information
    # Uncomment the following lines for debugging purposes
    # print(f"Process Name: {process_name}")
    # print(f"Thread Name Map: {thread_name_map}")

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
    thread_name_map (dict): Mapping from thread ID to thread name.

    Returns:
    pandas.DataFrame: DataFrame containing the CPU usage data.
    """
    data = pd.DataFrame(records)
    # Ensure thread_id is integer
    data["thread_id"] = data["thread_id"].astype(int)
    # Map thread_id to thread_name
    data["thread_name"] = data["thread_id"].map(thread_name_map).fillna("unknown")
    # Convert timestamp to datetime (assuming timestamp is seconds since epoch)
    data["timestamp"] = pd.to_datetime(data["timestamp"], unit="s")
    return data

def calculate_statistics(subset):
    """
    Calculate statistics (min, max, mean) for user and kernel CPU usage.

    Parameters:
    subset (pandas.DataFrame): Subset of data for a specific thread.

    Returns:
    dict: Calculated statistics.
    """
    stats = {}

    if "user_percent" in subset.columns:
        stats["min_user"] = subset["user_percent"].min()
        stats["max_user"] = subset["user_percent"].max()
        stats["mean_user"] = subset["user_percent"].mean()

    if "kernel_percent" in subset.columns:
        stats["min_kernel"] = subset["kernel_percent"].min()
        stats["max_kernel"] = subset["kernel_percent"].max()
        stats["mean_kernel"] = subset["kernel_percent"].mean()

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
    summary_lines = []
    summary_lines.append(f"Process Name: {process_name}")

    for thread_name in data["thread_name"].unique():
        subset = data[data["thread_name"] == thread_name]

        user_exists = "user_percent" in subset.columns
        kernel_exists = "kernel_percent" in subset.columns

        if (
            user_exists
            and subset["user_percent"].sum() == 0
            and kernel_exists
            and subset["kernel_percent"].sum() == 0
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
    data,
    process_name="Unknown Process",
    filter_thread=None,
    filter_cpu_type=None,
    time_range=None,
    show_summary_info=True,
):
    """
    Plot CPU usage over time for the process and its threads.

    Parameters:
    data (pandas.DataFrame): DataFrame containing CPU usage data.
    process_name (str): Name of the process.
    filter_thread (str, optional): Filter to include only specific thread names.
    filter_cpu_type (str, optional): Filter to include only 'user' or 'kernel' CPU usage.
    time_range (tuple, optional): Tuple of (start_time, end_time) to filter the data.
    show_summary_info (bool): Whether to display summary information on the plot.
    """
    plt.figure(figsize=(14, 10))

    # Calculate total CPU usage of the process
    process_cpu = calculate_process_cpu(data)

    # Sort by timestamp
    process_cpu = process_cpu.sort_values("timestamp")

    # Plot total CPU usage
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
    if filter_cpu_type:
        if filter_cpu_type.lower() == "user":
            data = data[["timestamp", "thread_name", "user_percent"]]
        elif filter_cpu_type.lower() == "kernel":
            data = data[["timestamp", "thread_name", "kernel_percent"]]

    if time_range:
        start_time, end_time = time_range
        data = data[(data["timestamp"] >= start_time) & (data["timestamp"] <= end_time)]

    # Sort data by timestamp
    data = data.sort_values("timestamp")

    # Set timestamp as index for resampling
    data.set_index("timestamp", inplace=True)

    # Determine resampling frequency based on data
    # Assuming the refresh_delay is consistent; if not, consider a fixed frequency like 'S' for seconds
    resample_freq = 'S'  # 1 second

    # Plot CPU usage for each thread
    for thread_name in data["thread_name"].unique():
        subset = data[data["thread_name"] == thread_name]

        # Resample to ensure continuity
        if filter_cpu_type:
            usage_column = "user_percent" if filter_cpu_type.lower() == "user" else "kernel_percent"
            usage = subset[usage_column].resample(resample_freq).mean().interpolate()
            plt.plot(
                usage.index,
                usage.values,
                label=f"{thread_name} ({filter_cpu_type.capitalize()})",
                linestyle="-",
            )
        else:
            # Plot both user and kernel CPU usage
            user_usage = subset["user_percent"].resample(resample_freq).mean().interpolate()
            kernel_usage = subset["kernel_percent"].resample(resample_freq).mean().interpolate()

            plt.plot(
                user_usage.index,
                user_usage.values,
                label=f"{thread_name} (User)",
                linestyle="-",
            )
            plt.plot(
                kernel_usage.index,
                kernel_usage.values,
                label=f"{thread_name} (Kernel)",
                linestyle=":",
            )

    plt.xlabel("Time")
    plt.ylabel("CPU Usage (%)")
    plt.title(f"CPU Usage Over Time by Thread for Process: {process_name}")
    plt.gcf().autofmt_xdate()
    plt.legend(loc="upper left", bbox_to_anchor=(1, 1))
    plt.grid(True)
    plt.tight_layout(rect=[0, 0.1, 1, 0.95])

    if show_summary_info:
        summary_info = get_summary_table(data.reset_index(), process_name)

        plt.figtext(
            0.02,
            0.01,
            summary_info,
            fontsize=9,
            verticalalignment="bottom",
            horizontalalignment="left",
            bbox=dict(facecolor="white", alpha=0.5),
        )

    try:
        plt.savefig("cpu_usage_over_time.png")
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
    parser.add_argument("--filter-thread", type=str, help="Filter by thread name.")
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

    args = parser.parse_args()

    try:
        # Read and parse the binary file
        records, process_name, thread_name_map = read_cpu_usage_bin(args.filename)
        if not records:
            print("No data found in the binary file.")
            return

        # Convert records to DataFrame
        data = parse_records_to_dataframe(records, thread_name_map)

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
        )

    except KeyboardInterrupt:
        print("\nExecution interrupted by user. Exiting gracefully.")
        sys.exit(0)
    except Exception as e:
        print(f"An error occurred: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
