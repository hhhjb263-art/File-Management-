# ---------------------------------------------------------------------------
#  云匣 CloudVault 服务端（C++17 / Linux / POSIX socket + SQLite3）
#
#  分层（禁止跨层反向依赖）：
#    core    基础能力：SHA-256、最小 JSON、日志、配置
#    meta    元数据：SQLite 连接、建表、Repository（依赖 core）
#    store   内容存储：内容寻址分块落盘（依赖 core）
#    net     HTTP 服务器：POSIX socket + 线程池（依赖 core）
#    app     应用装配：路由注册与启动
#
#  构建：
#    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#    cmake --build build -j
#
#  常用选项：
#    -DCV_BUILD_TESTS=ON|OFF        是否构建自检程序（默认 ON）
#    -DCV_INSTALL_TESTDATA=ON|OFF   是否安装测试数据目录（默认 OFF）
#    -DCV_ENABLE_ASAN=ON|OFF        开启 AddressSanitizer（默认 OFF）
#    -DCV_ENABLE_TSAN=ON|OFF        开启 ThreadSanitizer（默认 OFF，与 ASAN 互斥）
#    -DCMAKE_INSTALL_PREFIX=<dir>   安装前缀（默认 /usr/local）
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)

# 注意：VERSION 需与 src/app/main.cpp 中的 kVersion 保持同步（/healthz 接口会把 kVersion 返回给客户端）
project(cloudvault_server
  VERSION 0.5.0
  DESCRIPTION "云匣 CloudVault 个人私有云网盘服务端（C++17 / POSIX socket + SQLite3）"
  LANGUAGES CXX
)

# ---------------------------------------------------------------------------
#  全局编译设置
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# 导出 compile_commands.json，供 clangd / IDE 使用（仅 Makefile / Ninja 生成器生效）
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "导出 compile_commands.json" FORCE)

# ---------------------------------------------------------------------------
#  选项
# ---------------------------------------------------------------------------
option(CV_BUILD_TESTS "构建核心自检程序 cv_tests" ON)
option(CV_INSTALL_TESTDATA "安装 testdata 测试数据目录" OFF)
option(CV_ENABLE_ASAN "开启 AddressSanitizer（地址越界/内存泄漏检测）" OFF)
option(CV_ENABLE_TSAN "开启 ThreadSanitizer（数据竞争检测）" OFF)

# ASAN 与 TSAN 互斥，同时开启没有意义且会直接报错
if(CV_ENABLE_ASAN AND CV_ENABLE_TSAN)
  message(WARNING "CV_ENABLE_ASAN 与 CV_ENABLE_TSAN 互斥，已自动关闭 TSAN，仅保留 ASAN")
  set(CV_ENABLE_TSAN OFF CACHE BOOL "开启 ThreadSanitizer（数据竞争检测）" FORCE)
endif()

# ---------------------------------------------------------------------------
#  统一目标配置：C++17 特性 + 按编译器区分的警告选项 + Sanitizer
# ---------------------------------------------------------------------------
set(CV_SANITIZE_FLAGS "")

if(CV_ENABLE_ASAN)
  list(APPEND CV_SANITIZE_FLAGS -fsanitize=address -fno-omit-frame-pointer)
  message(STATUS "已开启 AddressSanitizer")
elseif(CV_ENABLE_TSAN)
  list(APPEND CV_SANITIZE_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
  message(STATUS "已开启 ThreadSanitizer")
endif()

# 为某个目标套用统一的编译特性、警告级别与 Sanitizer 选项
function(cv_target_setup target)
  target_compile_features(${target} PUBLIC cxx_std_17)

  # 警告选项按编译器区分：GNU/Clang 使用 -Wall -Wextra -Wpedantic，其余编译器不加
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  else()
    message(STATUS "[${target}] 编译器 ${CMAKE_CXX_COMPILER_ID} 非 GNU/Clang，跳过 -Wall/-Wextra/-Wpedantic")
  endif()

  if(CV_SANITIZE_FLAGS)
    target_compile_options(${target} PRIVATE ${CV_SANITIZE_FLAGS})
    target_link_options(${target} PRIVATE ${CV_SANITIZE_FLAGS})
  endif()
endfunction()

# ---------------------------------------------------------------------------
#  依赖（目标平台 Linux/POSIX；Windows 下 net 模块无法编译）
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)

# ---------------------------------------------------------------------------
#  core ----------------
# ---------------------------------------------------------------------------
add_library(cv_core STATIC
  src/core/sha256.cpp
  src/core/json.cpp
  src/core/logger.cpp
  src/core/config.cpp
)
target_include_directories(cv_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(cv_core PUBLIC Threads::Threads)
cv_target_setup(cv_core)

# ---------------------------------------------------------------------------
#  store ----------------
# ---------------------------------------------------------------------------
add_library(cv_store STATIC
  src/store/content_store.cpp
)
target_include_directories(cv_store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(cv_store PUBLIC cv_core)
cv_target_setup(cv_store)

# ---------------------------------------------------------------------------
#  meta ----------------
# ---------------------------------------------------------------------------
add_library(cv_meta STATIC
  src/meta/db.cpp
  src/meta/file_repository.cpp
  src/meta/upload_repository.cpp
)
target_include_directories(cv_meta PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(cv_meta PUBLIC cv_core SQLite::SQLite3)
cv_target_setup(cv_meta)

# ---------------------------------------------------------------------------
#  net ----------------
# ---------------------------------------------------------------------------
add_library(cv_net STATIC
  src/net/http_server.cpp
)
target_include_directories(cv_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(cv_net PUBLIC cv_core Threads::Threads)
cv_target_setup(cv_net)

# ---------------------------------------------------------------------------
#  app ----------------
# ---------------------------------------------------------------------------
add_executable(cloudvault-server src/app/main.cpp)
target_link_libraries(cloudvault-server PRIVATE cv_net cv_store cv_meta cv_core)
cv_target_setup(cloudvault-server)

# ---------------------------------------------------------------------------
#  tests ----------------
# ---------------------------------------------------------------------------
if(CV_BUILD_TESTS)
  # CTest 支持
  include(CTest)
  enable_testing()

  add_executable(cv_tests tests/test_core.cpp)
  target_link_libraries(cv_tests PRIVATE cv_core cv_store)
  cv_target_setup(cv_tests)

  add_test(NAME cv_tests COMMAND cv_tests)
endif()

# ---------------------------------------------------------------------------
#  安装
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS cloudvault-server
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# 测试数据只在显式要求时安装，避免污染生产环境
if(CV_INSTALL_TESTDATA AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/testdata)
  install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/testdata
    DESTINATION ${CMAKE_INSTALL_DATADIR}/cloudvault
  )
endif()

# ---------------------------------------------------------------------------
#  配置摘要
# ---------------------------------------------------------------------------
message(STATUS "----------------------------------------------------------")
message(STATUS "  ${PROJECT_NAME} ${PROJECT_VERSION}")
message(STATUS "  构建类型       : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ 标准       : C++17")
message(STATUS "  编译器         : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  SQLite3        : ${SQLite3_LIBRARIES}")
message(STATUS "  构建自检       : ${CV_BUILD_TESTS}")
message(STATUS "  安装测试数据   : ${CV_INSTALL_TESTDATA}")
message(STATUS "  AddressSanitizer: ${CV_ENABLE_ASAN}")
message(STATUS "  ThreadSanitizer: ${CV_ENABLE_TSAN}")
message(STATUS "  安装前缀       : ${CMAKE_INSTALL_PREFIX}")
message(STATUS "----------------------------------------------------------")
