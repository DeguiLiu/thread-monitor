cmake_minimum_required(VERSION 3.10)
project(thread_monitor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Common flags
add_compile_options(-Wall -Wextra -Wpedantic)

# Monitor tool
add_executable(cpu_monitor cpu_monitor.cc)
target_compile_options(cpu_monitor PRIVATE -O2)
target_link_libraries(cpu_monitor PRIVATE pthread)

# Dummy worker for testing
add_executable(dummy_worker dummy_worker.cc)
target_link_libraries(dummy_worker PRIVATE pthread)
