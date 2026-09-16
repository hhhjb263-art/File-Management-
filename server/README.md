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

**运行时选项（`--xxx` / 环境变量 `CV_XXX` / 配置文件 key）**

| 选项 | 默认值 | 说明 |
|---|---|---|
| `--data-dir` / `CV_DATA_DIR` / `data_dir` | `/var/lib/cloudvault` | 数据根目录（元数据库 / 内容库 / 临时分块） |
| `--files-root` / `CV_FILES_ROOT` / `files_root` | `<data-dir>/files` | **文件树允许根**：客户端只能在该目录内建目录 / 上传 / 下载。显式配置，**不随服务端启动目录（cwd）变化**；相对路径会按 cwd 转成绝对路径 |
| `--listen` / `CV_LISTEN` / `listen` | `0.0.0.0` | 监听地址 |
| `--port` / `CV_PORT` / `port` | `8080` | 监听端口 |
| `--workers` / `CV_WORKERS` / `workers` | `4` | HTTP 工作线程数 |
| `--chunk-size` / `CV_CHUNK_SIZE` / `chunk_size` | `5242880` | 分块大小（字节） |
| `--log-file` / `CV_LOG_FILE` / `log_file` | 空（stdout） | 日志文件 |
| `--log-level` / `CV_LOG_LEVEL` / `log_level` | `info` | `debug` / `info` / `warn` / `error` |
| `--auth-token` / `CV_AUTH_TOKEN` / `auth_token` | 空（**不启用**） | API Bearer Token；非空即启用鉴权（见 §4.3） |
| `--http` / `CV_HTTP` / `http` | `on` | 明文 HTTP 监听开关；`off` 仅保留 HTTPS（须配 TLS，否则启动报错，见 §4.3） |
| `--tls-port` / `CV_TLS_PORT` / `tls_port` | `0`（关） | HTTPS 监听端口（需 `--tls-cert` / `--tls-key`） |
| `--tls-cert` / `CV_TLS_CERT` / `tls_cert` | 空 | HTTPS PEM 证书路径 |
| `--tls-key` / `CV_TLS_KEY` / `tls_key` | 空 | HTTPS PEM 私钥路径 |
| `--data-key` / `CV_DATA_KEY` / `data_key` | 空（**不加密**） | 静态数据加密密钥文件（64 hex 或 32 字节原文）；非空即启用 blob 落盘加密（见 §4.5） |

> 想把文件树落到别处（如独立数据盘）：`--files-root=/mnt/data/cvfiles`。
> `/healthz` 会回显当前生效的 `data_dir` 与 `files_root`，便于核对。

**构建选项（CMake）**

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
| POST | `/api/v1/files` | 整文件上传（body 为原始字节，文件名取请求头 `X-CV-Name`，需 URL 编码） |
| GET | `/api/v1/files` | 文件列表（最近 500 条） |
| GET | `/api/v1/files/:id` | 文件元数据 |
| GET | `/api/v1/files/:id/content` | 下载（按分块顺序重组；支持 `Range: bytes=`，返回 `206` + `Content-Range`） |
| POST | `/api/v1/uploads/init` | 分块上传会话初始化（含秒传 / 断点复用） |
| GET | `/api/v1/uploads/:id` | 查询会话状态与已收分块 |
| PUT | `/api/v1/uploads/:id/chunk/:seq` | 上传单个分块（幂等，可带 `X-Chunk-SHA256`） |
| POST | `/api/v1/uploads/:id/complete` | 合并分块落库 |
| DELETE | `/api/v1/uploads/:id` | 取消会话 |
| POST | `/api/v1/dirs` | 创建目录（规范化 + 符号链接/越界拒绝） |
| GET | `/api/v1/dirs` | 列出已登记目录 |
| GET | `/api/v1/tree` | 嵌套文件树（目录在前/文件在后、name 升序；供客户端树选择） |
| GET | `/api/v1/storage` | 磁盘空间自查：`{data_dir,files_root,free_bytes,total_bytes,upload_safety_factor}` |
| GET | `/api/v1/download` | 按路径下载（仅限已记录文件，严格越界校验） |
| POST | `/api/v1/files/new` | 新建空文件 `{dir,name}`；同目录同名 → **409** |
| POST | `/api/v1/files/:id/rename` | 重命名 `{name}`；同名 → **409**；同名幂等返回 200 |
| DELETE | `/api/v1/files/:id` | 删除并**回收空间** → `200 {deleted:true,file_id,name,dir,freed_bytes,blobs_removed,disk_free_bytes}` |

**同名检测与覆盖**：
- 整文件上传 `POST /api/v1/files`、新建文件、重命名、分块 `init` 都会做**同目录同名检测**；
  已存在且未声明覆盖 → **`409 {"error":"name exists in target directory","exists":true,"name","dir","file_id"}`**，
  客户端据此询问用户。
- 用户确认覆盖后重发：整文件上传带请求头 **`X-CV-Overwrite: 1`**；分块上传在 `init` body 带 **`"overwrite": true`**
  （会话记在 `upload_flags` 表，`complete` 时替换同名记录内容）。覆盖成功返回 **200**（非 201）并带 `"overwritten": true`。

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

## 4.1 分块上传 + 断点续传（`/api/v1/uploads/*`）

适用于**大文件 / 弱网 / 可暂停续传**的场景。流程：`init` 拿到 `upload_id` → 逐块 `PUT` → `complete` 合并落库。客户端与服务端共用下方契约（已冻结）。

整文件上传（`POST /api/v1/files`）与分块上传是**两套独立协议**，互不干扰；两者落库后的文件都通过同一 `file_node` 表管理，整文件哈希定义一致（= 原始字节的 SHA-256），因此**分块上传完成的内容可被整文件秒传判重命中，反之亦然**。

### 状态表

`upload_session` 的 `status` 取以下值之一：`created`（已建会话未收块）、`uploading`（已收部分分块）、`completed`（已合并）、`aborted`（已取消/超时）。

### 端点

**`POST /api/v1/uploads/init`**　body：`{"name","size","chunk_size"（可选，缺省服务端配置，clamp 到 64KiB~64MiB）,"hash"（整文件 SHA-256，可选）}`
- 若 `hash` 已在内容库命中 → 秒传：返回 `{"upload_id":0,"done":true,"file_id":<已存在id>,...}`。
- 若同 `(hash,size,chunk_size)` 存在**未完成**会话 → 复用，返回其 `upload_id` 与 `uploaded` 列表（跨重启续传的关键）。
- 否则新建会话：返回 `{"upload_id","name","size","chunk_size","hash","uploaded":[],"received_bytes":0}`。
- 错误：`400` 参数非法（含 size<0、hash 非 64-hex）/ `413` 总量超过上限（默认 1 TiB sanity 上限）。

**`GET /api/v1/uploads/:id`**　返回 `{"upload_id","size","chunk_size","uploaded":[seq...],"received_bytes","status"}`；`404` 会话不存在。

**`PUT /api/v1/uploads/:id/chunk/:seq`**　body = 分块原始字节，可选头 `X-Chunk-SHA256` 落盘后比对。
- 校验：`seq` 越界 → `400`；非末尾分块大小必须 == `chunk_size`，末尾分块必须 `<= chunk_size` 且与剩余字节一致 → 否则 `400`；`X-Chunk-SHA256` 与落盘哈希不符 → `400`。
- 幂等：同一 `seq` 重复上传（内容相同/不同）均成功，覆盖旧记录，不产生重复行。
- 返回 `{"seq","received_bytes"}`；`404` 会话不存在 / `409` 会话已完成。

**`POST /api/v1/uploads/:id/complete`**　返回 `{"file_id","name","size","hash"}`。
- 缺分块 → `409` `{"missing":[...]}`；按 seq 流式算出的整文件哈希与 init 提供的 `hash` 不符 → `422` `{"invalid":[...]}`（坏分块 seq 列表，便于客户端只重传坏块）。
- 成功后将分块迁入内容库（内容寻址去重）、建 `file_node`、会话置 `completed`、清理临时分块文件。

**`DELETE /api/v1/uploads/:id`**　`204`（无 body）取消会话：置 `aborted`、删除临时分块、删除会话记录；`404` 不存在。

### 断点续传与自愈设计

- **持久化 + 重启可续传**：分块临时落 `<dataDir>/tmp/uploads/<upload_id>/<seq>.part`（先写 `.tmp` 再原子 `rename`）；会话进度存于 `upload_session` / `upload_chunk` 表。服务端重启后，客户端用相同 `hash` 重新 `init` 即可复用原会话。
- **DB 为真相源 + 自愈**：`GET`/`init` 返回 `uploaded` 时，对每个 DB 分块行**双重校验磁盘 `.part` 是否存在且大小匹配**；不一致则剔除该 DB 行（并回退 `status`），避免脏状态。
- **会话 GC**：启动时清理 `status ∈ {created,uploading}` 且 `updated_at` 超过 24h 的闲置会话（删临时文件 + 删记录），防止临时目录无限增长。
- **并发 / 幂等**：不同 `seq` 的 `PUT` 互不影响；同 `seq` 走 `ON CONFLICT(upload_id,seq) DO UPDATE`，重复上传安全幂等。

### 下载 Range 支持

`GET /api/v1/files/:id/content` 支持 `Range: bytes=start-end` 与 `bytes=start-`（含后缀 `bytes=-N`）：命中返回 `206` + `Accept-Ranges: bytes` + `Content-Range: bytes start-end/total`，并按分块 seek 读取**仅请求区间**（内存只约一个分块），不整文件入内存。

**镜像直读快路径（v0.9）**：若文件树镜像存在且大小与记录一致，Range 响应直接**顺序读镜像文件**
（比分块拼装少多次 blob 打开/seek/拷贝，且对 OS 预读与页缓存友好）；否则回退分块拼装。
日志以 `[镜像直读]` / `[分块拼装]` 区分来源，便于确认快路径是否生效。

### TLS / HTTPS（v0.9，双模式）

- **成熟库**：OpenSSL（构建期 `find_package(OpenSSL)` 自动探测；Debian 上 `apt install libssl-dev` 即可）。
  找不到 OpenSSL 时优雅降级为纯 HTTP（构建打 WARN），**绝不手写加密**。
- **双模式**：HTTP 与 HTTPS **可同时运行**（两个监听、同一套路由），按需选择：
  - 纯 HTTP：不配置 `--tls-*`（现状不变）
  - HTTP + HTTPS 并行：`--port=8080 --tls-port=8443 --tls-cert=... --tls-key=...`
  - HTTPS 端口独立于 HTTP 端口，二者开关互不影响
- **配置**（三种方式等价）：
  - 命令行：`--tls-port=<n>` / `--tls-cert=<pem>` / `--tls-key=<pem>`
  - 环境变量：`CV_TLS_PORT` / `CV_TLS_CERT` / `CV_TLS_KEY`
  - 配置文件：`tls_port` / `tls_cert` / `tls_key`
- **自签名证书**：`bash testdata/gen_cert.sh [CN] [IP]` 一键生成（含 SAN，10 年有效）；
  客户端需勾选【信任自签名证书】。
- **安全约束**：最低 TLS 1.2；私钥文件建议 `chmod 600`；用户显式要求 HTTPS 而二进制未编译 TLS 时
  **启动即报错退出**（静默降级是安全 footgun）。
- `/healthz` 回显 `tls_port`。

### 删除与空间回收（v0.8 修正）

`DELETE /api/v1/files/:id` 现在会**真正释放磁盘**：

1. `file_node.deleted = 1`（标记删除，列表不再返回）
2. 删除该文件的 `file_chunk` 链接行 + `file_dir` 归属行（不留悬空引用）
3. 逐个递减 `chunk.ref_count`；**引用计数归零且无任何 `file_chunk` 引用**的分块行被清理
   （安全网：即使计数漂移也不会误删仍在使用的 blob）
4. 删除这些分块的 blob 文件（`blobs/xx/<hash>`）+ 删除镜像文件
5. 响应返回实际释放字节数 `freed_bytes`、删除的 blob 数 `blobs_removed` 与当前 `disk_free_bytes`

> 去重共享的内容不会被误删：只要还有别的文件引用该分块，`ref_count > 0`，blob 保留。
> 覆盖上传（同名覆盖）走 `replaceContent`，同样会回收旧内容中归零的分块。

### 存储空间预检（v0.8）

- 上传前按 **1 倍文件大小 + 64 MiB 余量**预检 `dataDir` 剩余空间 —— `complete` 会把 tmp 分块
  **同盘 rename 搬进**内容库（`ContentStore::putFromFile`，零拷贝），因此**不再产生 tmp+blob 双份**；
  不足 → **`507 Insufficient Storage`** `{"error":"insufficient disk space on server","need_bytes","free_bytes","data_dir"}`
  （整文件上传与分块 `init` 都会拦，且**秒传命中 / 断点复用不做空间预检**——它们不额外占盘）。
- 镜像文件树写入前再查一次空间：不够则**跳过镜像**并 WARN（blob + DB 仍是权威，下载/预览不受影响）。
- `/healthz` 现在回显 `disk_free_bytes` / `disk_total_bytes`；`GET /api/v1/storage` 专门查询。
- 启动日志会打印剩余空间，低于 512 MiB 时给 WARN。

### 文件树镜像（v0.8 修复）

内容寻址存储里**只有分块 blob、没有「整文件 blob」**，因此镜像文件树必须按分块序列拼接：
`ContentStore::materializeFromChunks()`（先写 `.tmp` 再 rename，逐块读入，内存只约一个分块）。
旧实现用整文件哈希去 `materialize()` 必然失败（日志 `镜像文件树失败 ... blob missing: <整文件hash>`），
现已修复——整文件上传 / 分块 `complete` / 新建空文件 / 重命名兜底四处都走分块拼接。

### 大文件传输与内存保护（v0.7）

内存占用与文件大小**解耦**：服务端任何路径都不一次性加载整个文件。

| 规则 | 行为 |
|---|---|
| 整文件上传 `POST /api/v1/files` | 请求体 **> 64 MiB → `413`**，报文提示改用分块上传 `/api/v1/uploads/*` |
| 无 Range 的整文件下载 | 文件 **> 8 MiB → `409`**（`{"error":"...use Range requests","size":N,"max_single_shot":8388608}`），要求客户端分段拉取 |
| 分块上传 | 单请求体 = 1 个分块（默认 5 MiB，服务端 clamp 到 64 KiB~64 MiB）；总量上限 1 TiB（sanity 上限） |
| `complete` 合并 | 逐块读取拼接 + 流式算哈希，峰值 ≈ 1 个分块 |
| 带 Range 的下载 | `readRange` 按分块 seek，只读请求区间，峰值 ≈ 1 个分块 |

> 完整设计（适用场景 / 传输流程 / 内存策略 / 异常与中断处理 / 阈值总览）见
> [`docs/大文件传输设计.md`](../docs/大文件传输设计.md)。

### 跑分块冒烟脚本

```bash
# 先启动服务端（默认端口 8080）
./build/cloudvault-server --data-dir=/tmp/cvdata --port=8080

# 再运行（覆盖：init→逐块PUT→complete→下载回读、断点续传、秒传、空文件、Range）
./testdata/smoke_chunked.sh
BASE=http://127.0.0.1:9090 ./testdata/smoke_chunked.sh   # 指定端口
```
> 注意：Windows 下 git 不保留脚本执行位，请用 `bash testdata/smoke_chunked.sh` 显式运行；脚本仅依赖 `curl`/`sha256sum`/`dd`/`stat`，不依赖 `jq`。

## 4.2 目录树 + 按路径下载（边界受限）

**模型**：允许目录树的唯一物理根是 `<dataDir>/files/`；目录元数据在 `dir_node` 表
（DB 为真相源），上传完成的文件经硬链接镜像到 `<dataDir>/files/<dir>/<name>`
（跨设备退回复制）。**用户路径从不直接拼接磁盘路径**：先经 `core/path_util.h`
的 `sanitizeRelPath()` 清洗，再按 `(dir, name)` 查库取内容。

**端点**

| 方法 | 路径 | 请求 | 响应 |
|---|---|---|---|
| POST | `/api/v1/dirs` | JSON `{path}` | `201 {path,created:true}` 新建 / `200 {path,exists:true}` 幂等 |
| GET | `/api/v1/dirs` | — | `{"total":N,"items":[path,...]}` |
| GET | `/api/v1/tree` | — | `{"total_dirs":N,"total_files":M,"root":[<node>...]}`（见下） |
| GET | `/api/v1/download` | `?path=dir/name`（百分号编码）或头 `X-CV-Path` | `200` 全文 / `206` Range；未记录 `404`；非法 `400`；越界 `403` |

#### `GET /api/v1/tree`　嵌套文件树（供客户端文件树选择对话框）

不分页、不鉴权。把 `dir_node` 的全部目录路径 + `file_node` 的 `(dir,name,size,id)` 在内存里按路径段建树（**不写递归 SQL**）：

- 目录节点：`{"type":"dir","name","path","children":[...]}`；`path` 为规范化相对路径（`/` 分隔，根目录为空串不出现在树中）。
- 文件节点：`{"type":"file","name","path","size","id"}`；`path` 为 `dir/name`（根目录文件即 `name`）。
- **排序**：每层的子节点**目录在前、文件在后**，同类内按 `name` 字典序升序。
- **空目录必须出现**：无文件的目录以 `children:[]` 呈现（已被 `dir_node` 登记或仅含文件目录的祖先均会展开）。
- 统计：`total_dirs` = 非根目录节点数；`total_files` = 文件数（受 `listFiles` 的 500 条上限约束）。

建树数据源：`FileRepository::listDirsAll()`（已登记目录 + 文件所属目录及其全部祖先，去重后返回）与 `listFiles()`；目录/文件的父关系在 C++ 侧按路径段推导，key 上用 `"D:"`/`"F:"` 前缀隔离命名空间，避免同名目录与文件冲突。

```bash
curl -s localhost:8080/api/v1/tree
# 返回示例：
# {"total_dirs":2,"total_files":3,"root":[
#   {"type":"dir","name":"docs","path":"docs","children":[
#     {"type":"dir","name":"backup","path":"docs/backup","children":[]},
#     {"type":"file","name":"readme.txt","path":"docs/readme.txt","size":1234,"id":3}
#   ]},
#   {"type":"file","name":"a.txt","path":"a.txt","size":56,"id":1}
# ]}
```

**边界与校验规则**（`sanitizeRelPath` 统一裁决）：
- 拒绝 `.` / `..` 路径段（穿越）、绝对路径（`/` 开头）、盘符 / 冒号、反斜杠 → **403**
- 拒绝非法字符 `< > : " | ? *` 与控制字符、空路径段（`a//b`）、段尾 `.`/空格、
  Windows 保留设备名（`CON`/`NUL`/`COM1-9`/…，含 `CON.txt` 形态）→ **400**
- 限长：整路径 400 字节、单段 128 字节（UTF-8 计）
- 目录创建（`POST /api/v1/dirs`）在物理树上**逐级**创建：任何一级已存在且是符号
  链接 → **403**；创建后 `weakly_canonical` 包含校验，真实路径逃出允许根 → **403**
- 按路径下载对镜像树做防御纵深校验：文件本身是符号链接 → **403**；存在则包含校验
- 上传指定目录：整文件上传头 `X-CV-Dir`；分块上传 `init` body 带 `dir`（登记到
  `upload_dir` 表，`complete` 落位）。`dir` 为空 = 根目录；镜像失败仅告警
  （内容仍以 blob + DB 为权威）

curl 快速验证：

```bash
curl -s -X POST localhost:8080/api/v1/dirs -d '{"path":"docs/backup"}'      # 201
curl -s -X POST localhost:8080/api/v1/dirs -d '{"path":"../etc"}'           # 403 越界
curl -s -X POST localhost:8080/api/v1/dirs -d '{"path":"a//b"}'             # 400 非法
curl -s -X POST localhost:8080/api/v1/files -H 'X-CV-Name: a.txt' \
  -H 'X-CV-Dir: docs%2Fbackup' --data-binary @hello.txt                     # 201
curl -s "localhost:8080/api/v1/download?path=docs%2Fbackup%2Fa.txt" -o a.txt
curl -s "localhost:8080/api/v1/download?path=..%2Fsecret" -o -              # 403 越界
```

---

## 4.3 API 认证（Bearer Token）+ 明文 HTTP 关闭

抓包审计发现：TLS 只保护信道，不解决"谁来敲门"；且明文 8080 仍在监听。现增加应用层鉴权与明文端口开关。

### 配置（三通道等价：命令行 > 环境变量 > 配置文件 > 默认值）

| 选项 | 命令行 | 环境变量 | 配置文件键 | 默认 |
|---|---|---|---|---|
| API Token | `--auth-token=<secret>` | `CV_AUTH_TOKEN` | `auth_token` | 空（**不启用**） |
| 明文 HTTP | `--http=on\|off` | `CV_HTTP` | `http` | `on`（开） |

- **认证默认关闭**（向后兼容）。启动时按如下规则打日志（**绝不打印 token 明文**）：
  - 已配置 token → `INFO  鉴权已启用：除 /healthz 外全部接口强制 Authorization: Bearer <token>（同时兼容 X-CV-Token 头）`
  - 未配置 token + 已开 HTTPS 且监听地址 ≠ `127.0.0.1` → `WARN  未启用鉴权，任何能访问该端口(HTTPS) 的人都可读写全部文件`
  - 未配置 token 的其他情况 → `WARN  认证未启用：任何人可读写全部文件（生产环境请用 --auth-token）`
- **明文 HTTP 默认开启**：`--http=off` 仅关闭明文监听，仅保留 HTTPS。

### 行为

- 启用后，**除 `GET /healthz` 外所有请求**必须带 `Authorization: Bearer <token>`。
- 兼容历史头 `X-CV-Token: <token>`（直接携带 token，无 `Bearer ` 前缀）；两种头任选其一即可。
- 缺失 / 不匹配 → `401`，**响应体说明原因，且不再 dispatch 后续处理**：
  ```json
  {"error":"missing or invalid Authorization token (use: Authorization: Bearer <token>)","status":401}
  ```
- **恒定时间比较**：Token 比对不用 `==`，而是先异或折叠长度差、再对公共前缀逐字节异或累加差异，且**不提前 return**，比较时长与首个不同字节位置无关，避免时序侧信道。`/healthz` 始终免认证（保持探活）。
- `GET /healthz` 免认证（保持探活），但**启用认证时不再返回 `data_dir` / `files_root`**（避免泄露服务器路径），仅返回 `status` / `version` / `tls_port` / `disk_free_bytes` / `disk_total_bytes`。
- `--http=off` 且未配置 `--tls-port`（或 TLS 未编译 / 证书缺失）→ **启动直接报错退出**，杜绝"零监听"静默状态；TLS 证书缺失 / 未编译时报错信息亦明确。

```bash
# 启用 Token 鉴权（任选一种配置通道）
./cloudvault-server --auth-token='s3cr3t' --tls-port=8443 --tls-cert=cert.pem --tls-key=key.pem

# 不带 Token 访问 → 401
curl -s localhost:8443/api/v1/files
# {"error":"unauthorized","status":401}

# 带 Token → 200
curl -s -H 'Authorization: Bearer s3cr3t' localhost:8443/api/v1/files

# 关闭明文 HTTP，仅留 HTTPS
./cloudvault-server --auth-token='s3cr3t' --http=off --tls-port=8443 --tls-cert=cert.pem --tls-key=key.pem

# 仅关 HTTP 却不配 TLS → 启动失败并明确报错
./cloudvault-server --http=off
# [error] 配置冲突：--http=off（明文 HTTP 已关闭）但未配置 --tls-port，...
```

---

## 4.4 keep-alive（连接复用）

每连接默认支持 keep-alive：一个 worker 线程全程持有该连接，循环「读请求 → 处理 → 写响应」，
避免每个请求都重建 TCP/TLS 握手（抓包实测原实现每请求一次完整握手、无任何会话复用）。
仅支持 `Content-Length` 定长帧（与现有实现一致），不引入 chunked。

**响应头**：可继续时 `Connection: keep-alive`，要关闭时显式 `Connection: close`（HTTP/1.1 默认 keep-alive，仍显式写出以最大化兼容性）。

**关闭本连接的触发条件**：

- 客户端请求头带 `Connection: close`；
- 请求解析失败（返回 `400`）；
- 单连接累计请求数达到上限（默认 `200`，超出后本次响应标记关闭，下一轮直接断开）；
- 读下一请求超时（SO_RCVTIMEO = 30s）或对端关闭 —— **超时静默关闭，不刷 WARN**，避免日志被空连接刷屏。

**关键正确性**：keep-alive 下 `recv` 可能一次多读「下一请求的开头字节」。实现会把当前请求
之后的残留字节保留为 `residue`，作为下一次 `readRequest` 的起点，否则后续请求会解析错乱。
`readRequest` 在拼出「恰好一个完整请求」后才把多余字节切出留给下一轮，且 `raw` 被截断到本请求边界再交给 `parseRequest`。

> 现有「请求队列 + worker 线程池」模型不变：一个连接仍由一个 worker 全程持有，不改为长连接跨 worker。

## 4.5 静态数据加密（blob 落盘加密）

目标：内容库 blob 落盘加密，拿到磁盘也无法直接读出文件内容。默认关闭（向后兼容），
配置密钥后即启用。

### 配置（三通道等价）

| 选项 | 命令行 | 环境变量 | 配置文件键 | 默认 |
|---|---|---|---|---|
| 静态加密密钥 | `--data-key=<path>` | `CV_DATA_KEY` | `data_key` | 空（**不加密**） |

- 密钥文件支持两种格式：**64 个十六进制字符（32 字节）**，或 **32 字节原始数据**（两种都支持，自动识别；文件首尾空白/换行会被忽略）。
- 密钥文件不存在 / 长度不对（既非 64 hex 也非 32 字节）→ **启动报错退出**。
- 已配置 `--data-key` 但二进制未编译 OpenSSL（`CV_HAVE_OPENSSL` 未定义）→ **启动报错退出**（绝不手写加密）。

启用示例：

```bash
# 生成 32 字节随机密钥（hex 形式）
openssl rand -hex 32 > /etc/cloudvault/data.key
./cloudvault-server --data-key=/etc/cloudvault/data.key --tls-port=8443 --tls-cert=cert.pem --tls-key=key.pem
```

### 加密格式

blob 文件落盘布局（每个 blob 独立随机 nonce）：

```
magic(4 字节 "CVB1") + nonce(12) + tag(16) + ciphertext
```

- 算法：**OpenSSL EVP AES-256-GCM**；每 blob 独立随机 12 字节 nonce；`tag` 用于解密时校验完整性与真实性（被篡改会解密失败）。
- **只加密内容，不加密文件名 / 哈希**：blob 文件名仍是明文 sha256 十六进制（用于内容寻址与秒传），文件名本身不含机密。
- 覆盖全部读写路径：`put` / `putFromFile` / `get` / `readRange` / `materializeFromChunks`；`getChunked`、`readRange`（GCM 不可 seek → 先整块解密再切片，blob 即分块、通常 ≤5MiB，可接受）均经 `get` 自然解密。
- `putFromFile` 在未加密时是「同盘 rename 零拷贝搬移」；加密开启后该优化**自然失效**：必须先读明文再加密落盘（源文件随后清理），未加密时仍保留原零拷贝行为。

### 两个必须知晓的取舍（开启加密时自动生效）

1. **文件树镜像落盘自动关闭**：镜像本质是明文副本，会让加密形同虚设。故启用加密后，物理镜像树不再生成；下载走分块拼装路径（见下）。启动会打一条 `WARN` 说明此取舍。
2. **Range 下载自动走「分块拼装 + 解密」**：镜像直读快路径（`serveFileContent` 的快捷分支）在加密开启时被跳过，Range 下载统一经 `readRange` 分块拼装并解密，日志显示 `[分块拼装]`。单次吞吐略降，但安全性提升。

### 自查

加密生效后，落盘 blob 不得含明文片段：

```bash
# 应无任何有意义的明文输出（strings 找不到可读片段）
strings blobs/xx/<sha256> | head
```

---

## 5. 今天做 / 没做的边界

**已实现**

- 分层工程骨架 + CMake + 核心自检
- 配置（命令行 > 环境变量 > 配置文件 > 默认值）、结构化日志
- HTTP/1.1 服务器：POSIX socket + 线程池 + 路径参数路由；支持 **keep-alive**（单连接复用、上限 200 请求、读超时静默关闭、完整切走上一请求 body 残留），不再每请求一次握手（见 §4.4）
- SHA-256 内容寻址存储：5MB 分块、原子写入（临时文件 + rename）、去重
- SQLite 元数据：chunk / file_node / file_chunk / file_version / upload_session / upload_chunk / dir_node / file_dir / upload_dir 建表，写入走事务
- 整文件上传 / 列表 / 下载（下载支持 `Range: bytes=` → `206`）
- 分块上传协议 + 断点续传 + 秒传：`init` → `PUT chunk` → `complete` → `DELETE`，含会话复用、DB+磁盘双重校验自愈、闲置会话 GC
- 端到端冒烟脚本 `testdata/smoke_chunked.sh`（覆盖续传 / 秒传 / 空文件 / Range）
- 边界受限目录树：`POST/GET /api/v1/dirs`、嵌套文件树 `GET /api/v1/tree`、按路径下载 `GET /api/v1/download`
  （路径规范化清洗、`..`/绝对路径/符号链接拒绝、越界 403）
- API 鉴权：Bearer Token（`--auth-token` / `CV_AUTH_TOKEN` / `auth_token`，兼容 `X-CV-Token` 头；恒定时间比较，不提前 return），除 `/healthz` 外全接口强制 401（JSON 说明原因，不再 dispatch）；明文 HTTP 可按 `--http=off` 关闭（必须与 TLS 二选一，否则启动报错）
- 静态数据加密：blob 落盘 AES-256-GCM（每 blob 独立随机 nonce，`magic+nonce+tag+ciphertext` 格式；只加密内容、不加密文件名/哈希）；`--data-key` / `CV_DATA_KEY` / `data_key` 配置，覆盖 `put`/`putFromFile`/`get`/`readRange`/`materializeFromChunks` 全路径；启用后自动关闭文件树明文镜像、Range 走分块拼装 + 解密（见 §4.5）

**明确未实现（后续模块）**

- 用户、登录、会话、配额
- 分享链接、回收站、版本回溯（表结构已预留）
- WebSocket 同步事件推送
- HTTPS（生产环境应在前面挂 Caddy / Nginx 终止 TLS）
- 下载的「流式」输出仍在应用层按区间拼装（受 net 层 `resp.body` 模型限制）；超大文件全文下载仍整文件入内存，未来可在 net 层引入分块流式回应

---

## 6. 下一步模块顺序建议

| 模块 | 内容 | 依赖 |
|---|---|---|
| 模块 2 | ~~分块上传协议（已完成）~~ | 模块 1 |
| 模块 3 | 用户与会话：用户表、密码哈希、登录 Token、踢下线 | 模块 1 |
| 模块 4 | 目录树（parent_id）、回收站（软删除）、版本回溯 | 模块 3 |
| 模块 5 | 分享链接（签名 URL + 提取码 + 次数限制） | 模块 4 |
| 模块 6 | WebSocket 事件推送 + 客户端同步对接 | 模块 3 |

---

## 7. 备注

- 服务端语言由原技术栈文档的 Go 改为 **C++17**（本机为 Qt 工程，统一语言降低心智负担）；
  `docs/TECH-STACK-技术选型（按优先级）.md` 的 ADR-1 已同步更新。
- 代码目标平台为 Linux（POSIX socket）。Windows 上无法直接编译 net 模块。
