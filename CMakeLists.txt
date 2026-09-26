cmake_minimum_required(VERSION 3.20)
project(llm_gateway LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

add_library(gateway_core STATIC src/wol.cpp src/health.cpp third_party/cpp-httplib/httplib.cpp)
target_include_directories(gateway_core PRIVATE third_party/cpp-httplib)
target_link_libraries(gateway_core PRIVATE Threads::Threads)
target_compile_options(gateway_core PRIVATE -Wall -Wextra)

add_executable(wake-and-wait src/wake_and_wait.cpp)
target_link_libraries(wake-and-wait PRIVATE gateway_core)
target_compile_options(wake-and-wait PRIVATE -Wall -Wextra)
