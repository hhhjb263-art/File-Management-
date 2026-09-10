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
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)
project(cloudvault_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")

find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)

# ---------------- core ----------------
add_library(cv_core STATIC
  src/core/sha256.cpp
  src/core/json.cpp
  src/core/logger.cpp
  src/core/config.cpp
)
target_include_directories(cv_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(cv_core PUBLIC Threads::Threads)

# ---------------- store ----------------
add_library(cv_store STATIC
  src/store/content_store.cpp
)
target_include_directories(cv_store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(cv_store PUBLIC cv_core)

# ---------------- meta ----------------
add_library(cv_meta STATIC
  src/meta/db.cpp
  src/meta/file_repository.cpp
)
target_include_directories(cv_meta PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(cv_meta PUBLIC cv_core SQLite::SQLite3)

# ---------------- net ----------------
add_library(cv_net STATIC
  src/net/http_server.cpp
)
target_include_directories(cv_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(cv_net PUBLIC cv_core Threads::Threads)

# ---------------- app ----------------
add_executable(cloudvault-server src/app/main.cpp)
target_link_libraries(cloudvault-server PRIVATE cv_net cv_store cv_meta cv_core)

# ---------------- tests ----------------
option(CV_BUILD_TESTS "Build self tests" ON)
if(CV_BUILD_TESTS)
  add_executable(cv_tests tests/test_core.cpp)
  target_link_libraries(cv_tests PRIVATE cv_core cv_store)
endif()
