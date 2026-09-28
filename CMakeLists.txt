cmake_minimum_required(VERSION 3.20)
project(llm_gateway LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

add_library(httplib STATIC third_party/cpp-httplib/httplib.cpp)
target_include_directories(httplib PUBLIC third_party/cpp-httplib)
target_link_libraries(httplib PUBLIC Threads::Threads)

add_library(gateway_core STATIC src/wol.cpp src/health.cpp src/config.cpp src/net_guard.cpp src/auth.cpp)
target_link_libraries(gateway_core PRIVATE httplib json)
target_compile_options(gateway_core PRIVATE -Wall -Wextra)

enable_testing()

add_executable(test_net_guard tests/test_net_guard.cpp)
target_link_libraries(test_net_guard PRIVATE gateway_core)
target_include_directories(test_net_guard PRIVATE src)
add_test(NAME net_guard COMMAND test_net_guard)

add_library(json INTERFACE)
target_include_directories(json INTERFACE third_party)

add_executable(wakey src/gateway.cpp)
target_link_libraries(wakey PRIVATE gateway_core httplib json)
target_compile_options(wakey PRIVATE -Wall -Wextra)

add_executable(wake-and-wait src/wake_and_wait.cpp)
target_link_libraries(wake-and-wait PRIVATE gateway_core)
target_compile_options(wake-and-wait PRIVATE -Wall -Wextra)
