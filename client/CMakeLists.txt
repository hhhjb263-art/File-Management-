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
#
# ---------------------------------------------------------------------------
#  Windows（MinGW 工具链，必须与 Qt 套件成套匹配；注意 cmd 续行符是 ^）
#
#    cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release ^
#      -DCMAKE_PREFIX_PATH=D:/Qt/6.9.3/mingw_64 ^
#      -DCMAKE_CXX_COMPILER=D:/Qt/Tools/mingw1310_64/bin/g++.exe ^
#      -DCMAKE_MAKE_PROGRAM=D:/Qt/Tools/mingw1310_64/bin/mingw32-make.exe
#    cmake --build build -j
#
#    构建后把 Qt 运行时 DLL 部署到 exe 旁边（之后双击即可运行，无需改 PATH）：
#      cmake --build build --target deploy
#
#  Linux 产物是 ELF、Windows 产物是 PE，链接的 Qt 运行时也不同，二者不可互换：
#  同一份源码需要在两个平台各自编译一次。源码本身无平台相关代码。
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
  src/FileTreeDialog.h
  src/FileTreeDialog.cpp
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

# Windows 下把目标标记为 GUI 程序（PE 子系统 = Windows GUI）。
# 不设这一属性，链接出的 exe 是 Console 子系统，双击运行时会在主窗口之外
# 多弹一个黑色控制台窗口。该属性在 Linux / macOS 上被 CMake 忽略，无副作用。
# 注意：Qt 只有 qt_add_executable() 会自动设置它，普通 add_executable() 必须手写。
set_target_properties(cloudvault-client PROPERTIES
  WIN32_EXECUTABLE TRUE
)

# Windows（可选）：把 Qt 运行时 DLL 部署到 exe 同目录，拷贝后双击即可运行，
# 不必再手动把 <Qt>/bin 加进 PATH。用 find_program 探测，找不到就静默跳过，
# 不会影响正常构建。
#   cmake --build build --target deploy
if(WIN32)
  get_filename_component(_qt_bin_dir "${Qt6_DIR}/../../../bin" ABSOLUTE)
  find_program(WINDEPLOYQT_EXECUTABLE
    NAMES windeployqt windeployqt6
    HINTS "${_qt_bin_dir}"
  )
  if(WINDEPLOYQT_EXECUTABLE)
    add_custom_target(deploy
      COMMAND "${WINDEPLOYQT_EXECUTABLE}" "$<TARGET_FILE:cloudvault-client>"
      DEPENDS cloudvault-client
      COMMENT "windeployqt: 为 cloudvault-client 部署 Qt 运行时"
      VERBATIM
    )
  else()
    message(STATUS "未找到 windeployqt，已跳过 deploy 目标；"
                   "可手动运行 <Qt>/<版本>/<工具链>/bin/windeployqt.exe")
  endif()
endif()
