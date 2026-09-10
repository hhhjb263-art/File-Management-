# 云匣 CloudVault 服务端（C++17 / Linux）

零依赖 POSIX + SQLite3 实现的服务端骨架。**今日完成模块 0（骨架）+ 模块 1（存储）**，
走通一条最小闭环：上传 → 分块落盘 → 元数据入库 → 秒传命中 → 按块重组下载。

---

## 1. 目录结构（分层，禁止跨层反向依赖）

```
server/
├── CMakeLists.txt
├── src/
│   ├── core/     基础能力：SHA-256 / 最小 JSON / 日志 / 配置 / 小工具
│   ├── meta/     元数据：SQLite 封装、建表脚本、文件 Repository
│   ├── store/    内容存储：内容寻址分块落盘
│   ├── net/      HTTP 服务器：POSIX socket + 固定线程池
│   └── app/      main.cpp：装配与路由
└── tests/        test_core.cpp：SHA-256 / JSON / 存储 自检
```

依赖方向：`app → {net, meta, store} → core`，core 不依赖任何上层。

---

## 2. Linux 构建（编译命令）

> 目标平台为 **Linux / POSIX**（`net` 模块用 POSIX socket 实现），**Windows 下无法编译**。
> 需要 CMake ≥ 3.16 与支持 C++17 的编译器。

### 2.1 安装依赖

```bash
# Debian / Ubuntu
sudo apt install -y build-essential cmake libsqlite3-dev

# RHEL / CentOS / Rocky / Fedora
sudo yum install -y gcc-c++ cmake sqlite-devel        # Fedora 也可用 dnf
```

### 2.2 基础构建（Release，推荐）

```bash
cd server
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

产物：

- `build/cloudvault-server` —— 服务端
- `build/cv_tests` —— 核心自检（SHA-256 向量、JSON 往返、分块读写）
- `build/compile_commands.json` —— 编译数据库（用 `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` 获得，已默认开启）
  供 clangd / VSCode / CLion 等 IDE 做跳转与补全；clangd 用户可把它软链到项目根：`ln -s build/compile_commands.json compile_commands.json`

### 2.3 跑自检

```bash
# 方式一：CTest（构建时已用 add_test(cv_tests) 注册，测试名就叫 cv_tests）
ctest --test-dir build --output-on-failure

# 方式二：直接运行（期望输出：全部自检通过）
./build/cv_tests
```

> `ctest --test-dir` 需要 CMake/CTest ≥ 3.20；老版本改用 `cd build && ctest --output-on-failure`。

不想要自检目标时用 `-DCV_BUILD_TESTS=OFF` 关闭（此时不会生成 `cv_tests`，也就没有 CTest 用例）：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCV_BUILD_TESTS=OFF
cmake --build build -j$(nproc)
```

### 2.4 调试构建

```bash
# Debug：-O0 -g，便于 gdb 单步
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j$(nproc)

# RelWithDebInfo：-O2 -g，兼顾性能与调试信息
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-rel -j$(nproc)
```

### 2.5 Sanitizer 构建（排查内存 / 并发问题）

```bash
# AddressSanitizer：地址越界、use-after-free、内存泄漏
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCV_ENABLE_ASAN=ON
cmake --build build-asan -j$(nproc)
ctest --test-dir build-asan --output-on-failure

# ThreadSanitizer：数据竞争
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DCV_ENABLE_TSAN=ON
cmake --build build-tsan -j$(nproc)
ctest --test-dir build-tsan --output-on-failure
```

> **`CV_ENABLE_ASAN` 与 `CV_ENABLE_TSAN` 互斥**：两者不能同时使用。
> 若同时传 `-DCV_ENABLE_ASAN=ON -DCV_ENABLE_TSAN=ON`，CMake 会打印一条 WARNING
> 并**自动关闭 TSAN、只保留 ASAN**。Sanitizer 建议配 Debug 构建使用。

### 2.6 安装

```bash
# 默认安装前缀 /usr/local；可执行文件装到 <prefix>/bin
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build -j$(nproc)
sudo cmake --install build

# 顺带装上测试数据（默认 OFF，安装到 <prefix>/share/cloudvault/testdata）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local -DCV_INSTALL_TESTDATA=ON
cmake --build build -j$(nproc)
sudo cmake --install build
```

安装到用户目录（无需 sudo）：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$HOME/.local
cmake --build build -j$(nproc)
cmake --install build
```

### 2.7 可配置项一览

| 选项 | 默认值 | 说明 |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `Release` | `Release` / `Debug` / `RelWithDebInfo` / `MinSizeRel` |
| `CV_BUILD_TESTS` | `ON` | 是否构建自检目标 `cv_tests` 并注册 CTest 用例 |
| `CV_INSTALL_TESTDATA` | `OFF` | 是否把 `testdata/` 安装到 `<prefix>/share/cloudvault` |
| `CV_ENABLE_ASAN` | `OFF` | 开启 AddressSanitizer |
| `CV_ENABLE_TSAN` | `OFF` | 开启 ThreadSanitizer（与 `CV_ENABLE_ASAN` 互斥） |
| `CMAKE_INSTALL_PREFIX` | `/usr/local` | 安装前缀 |
| `CMAKE_EXPORT_COMPILE_COMMANDS` | `ON` | 生成 `build/compile_commands.json` |

### 2.8 一键复制版

```bash
# 依赖 → 构建 → 自检 → 安装，一条龙
sudo apt install -y build-essential cmake libsqlite3-dev && \
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && \
cmake --build build -j$(nproc) && \
ctest --test-dir build --output-on-failure && \
sudo cmake --install build
```

---

## 3. 运行

```bash
mkdir -p /tmp/cvdata
./build/cloudvault-server --data-dir=/tmp/cvdata --port=8080 --workers=4

# 或者用配置文件
cat > /etc/cloudvault.conf <<EOF
data_dir=/srv/cloudvault
port=8080
workers=4
chunk_size=5242880
log_level=info
log_file=/var/log/cloudvault.log
EOF
./build/cloudvault-server --config=/etc/cloudvault.conf

# 查看全部参数
./build/cloudvault-server --help
```

数据目录布局（自动创建）：

```
/tmp/cvdata/
├── meta/cloudvault.db     # 元数据（SQLite，WAL 模式）
├── blobs/ab/cdef...       # 内容寻址块，键 = SHA-256 前 2 位 / 完整哈希
└── tmp/
```

---

## 4. 接口与 curl 验证

`testdata/` 内置了现成的测试数据与一键冒烟脚本：

| 文件 | 用途 |
|---|---|
| `hello.txt` | 最小上传样例（内容含中文） |
| `notes.txt` | 多行中文内容，验证 body 原样透传与中文文件名 URL 解码 |
| `dup-a.txt` / `dup-b.txt` | 内容完全相同，验证**秒传命中**（两者 SHA-256 均为 `916acd58…67e`） |
| `empty.txt` | 0 字节边界用例 |
| `smoke.sh` | 一键冒烟：健康检查 → 上传 → 秒传 → 中文文件名 → 下载回读 diff → 空文件 → 6MB 跨分块 → 列表 |

```bash
# 先启动服务端，然后：
./testdata/smoke.sh                       # 全部通过则输出"冒烟测试全部通过"
BASE=http://127.0.0.1:9090 ./testdata/smoke.sh   # 指定其他端口
# 小文件也想过分块？启动时把分块调小：--chunk-size=64，hello.txt 会切成多块
```

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/healthz` | 健康检查 |
| POST | `/api/v1/files` | 上传（body 为原始字节，文件名取请求头 `X-CV-Name`，需 URL 编码） |
| GET | `/api/v1/files` | 文件列表（最近 500 条） |
| GET | `/api/v1/files/:id` | 文件元数据 |
| GET | `/api/v1/files/:id/content` | 下载（按分块顺序重组） |

```bash
# 1) 健康检查
curl -s http://127.0.0.1:8080/healthz

# 2) 造个测试文件并上传
head -c 12582912 /dev/urandom > /tmp/demo.bin        # 12MB → 应切成 3 块
curl -s -X POST --data-binary @/tmp/demo.bin \
     -H "X-CV-Name: $(python3 -c "import urllib.parse;print(urllib.parse.quote('演示文件.bin'))")" \
     http://127.0.0.1:8080/api/v1/files
# 返回示例：{"id":1,"name":"演示文件.bin","size":12582912,"hash":"...","chunks":3,"instant":false}

# 3) 再传一次同样内容 —— 秒传命中
curl -s -X POST --data-binary @/tmp/demo.bin \
     -H "X-CV-Name: demo-copy.bin" \
     http://127.0.0.1:8080/api/v1/files
# 返回 "instant":true，且 blobs 目录不会新增块

# 4) 列表 / 下载 / 校验哈希一致
curl -s http://127.0.0.1:8080/api/v1/files
curl -s -o /tmp/out.bin http://127.0.0.1:8080/api/v1/files/1/content
sha256sum /tmp/demo.bin /tmp/out.bin        # 两个哈希必须相同
```

观察落盘：`find /tmp/cvdata/blobs -type f | head`，12MB 文件切 3 块，第二次上传不产生新块。

---

## 5. 今天做 / 没做的边界

**已实现**

- 分层工程骨架 + CMake + 核心自检
- 配置（命令行 > 环境变量 > 配置文件 > 默认值）、结构化日志
- HTTP/1.1 服务器：POSIX socket + 线程池 + 路径参数路由（每连接单请求后关闭）
- SHA-256 内容寻址存储：5MB 分块、原子写入（临时文件 + rename）、去重
- SQLite 元数据：chunk / file_node / file_chunk / file_version 建表，写入走事务

**明确未实现（后续模块）**

- 分块上传协议与断点续传（现在是整文件 body，256MB 上限）
- 用户、登录、会话、配额
- 分享链接、回收站、版本回溯（表结构已预留）
- WebSocket 同步事件推送
- HTTPS（生产环境应在前面挂 Caddy / Nginx 终止 TLS）

---

## 6. 下一步模块顺序建议

| 模块 | 内容 | 依赖 |
|---|---|---|
| 模块 2 | 分块上传协议：`POST /uploads/init` → `PUT /uploads/:id/chunk/:seq` → `POST /uploads/:id/complete`，支持断点续传与秒传 | 模块 1 |
| 模块 3 | 用户与会话：用户表、密码哈希、登录 Token、踢下线 | 模块 1 |
| 模块 4 | 目录树（parent_id）、回收站（软删除）、版本回溯 | 模块 3 |
| 模块 5 | 分享链接（签名 URL + 提取码 + 次数限制） | 模块 4 |
| 模块 6 | WebSocket 事件推送 + 客户端同步对接 | 模块 3 |

---

## 7. 备注

- 服务端语言由原技术栈文档的 Go 改为 **C++17**（本机为 Qt 工程，统一语言降低心智负担）；
  `docs/TECH-STACK-技术选型（按优先级）.md` 的 ADR-1 已同步更新。
- 代码目标平台为 Linux（POSIX socket）。Windows 上无法直接编译 net 模块。
