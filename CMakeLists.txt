cmake_minimum_required(VERSION 3.10)
project(thread_monitor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Common flags
add_compile_options(-Wall -Wextra -Wpedantic -Werror)

# Sanitizer options
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

if(ENABLE_ASAN)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()

if(ENABLE_UBSAN)
  add_compile_options(-fsanitize=undefined)
  add_link_options(-fsanitize=undefined)
endif()

# Monitor tool
add_executable(cpu_monitor cpu_monitor.cc)
target_compile_options(cpu_monitor PRIVATE -O2)
target_link_libraries(cpu_monitor PRIVATE pthread)

# Dummy worker for testing
add_executable(dummy_worker dummy_worker.cc)
target_link_libraries(dummy_worker PRIVATE pthread)

# Unit tests (optional, requires test file)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_proc_parser.cc")
  enable_testing()
  add_executable(test_proc_parser test_proc_parser.cc)
  target_link_libraries(test_proc_parser PRIVATE pthread)
  add_test(NAME proc_parser_tests COMMAND test_proc_parser)
endif()
