# ---------------------------------------------------------------------------
#  云匣 CloudVault 测试客户端（Qt 6 Widgets + Network，C++17）
#
#  构建：
#    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#    cmake --build build -j
#
#  运行前请先启动服务端：
#    ../server/build/cloudvault-server --data-dir=/tmp/cvdata --port=8080
#
#  若 Qt 不在默认搜索路径，用 CMAKE_PREFIX_PATH 指定：
#    cmake -S . -B build -DCMAKE_PREFIX_PATH=D:/Qt/6.9.3/mingw_64
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)

project(cloudvault_client
  VERSION 1.0.0
  DESCRIPTION "云匣 CloudVault 的 Qt 6 测试客户端（按钮驱动验证服务端接口）"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Qt 的 moc / uic / rcc 自动处理
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

add_executable(cloudvault-client
  src/main.cpp
  src/MainWindow.h
  src/MainWindow.cpp
)

target_link_libraries(cloudvault-client
  PRIVATE
    Qt6::Widgets
    Qt6::Network
)

# 源码统一按 UTF-8 处理，保证界面上的中文正常显示
if(MSVC)
  target_compile_options(cloudvault-client PRIVATE /utf-8)
else()
  target_compile_options(cloudvault-client PRIVATE -Wall -Wextra)
endif()
