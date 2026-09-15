# 云匣 CloudVault 测试客户端（Qt 6）

一个**按钮驱动**的 Qt Widgets 小程序，用来人工验证服务端接口是否正常工作。
不依赖任何第三方库，只使用 Qt 自带的 `Qt6::Widgets` + `Qt6::Network` + `QJsonDocument`。

对应的服务端请把 `server/README.md` 与 `server/src/app/main.cpp` 当作接口契约来源。

---

## 1. 目录结构

```
client/
├── CMakeLists.txt                 # cmake -S . -B build
├── README.md                      # 本文件
├── src/
│   ├── main.cpp                   # QApplication 入口
│   ├── MainWindow.h
│   └── MainWindow.cpp             # 界面 + 网络请求
└── testdata/                      # 测试数据（见第 5 节）
    ├── hello.txt
    ├── 中文文件名测试.txt
    ├── dup-a.txt / dup-b.txt
    ├── empty.txt
    ├── gen_big.sh                 # 生成约 6MB 大文件（Linux / macOS / Git Bash）
    └── gen_big.bat                # 生成约 6MB 大文件（Windows cmd）
```

---

## 2. 依赖

### Linux（Debian / Ubuntu）

```bash
sudo apt install -y build-essential cmake qt6-base-dev
```

### Linux（RHEL / CentOS / Rocky / Fedora）

```bash
sudo dnf install -y gcc-c++ cmake qt6-qtbase-devel
```

### Windows

- Qt 6.x（勾选 **Qt Widgets / Qt Network**，MinGW 或 MSVC 均可）
- CMake ≥ 3.16
- 编译器：MinGW（Qt 自带的 `Tools/mingw*_64`）或 MSVC 2022

若 CMake 找不到 Qt，构建时用 `CMAKE_PREFIX_PATH` 指定 Qt 安装前缀：

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=D:/Qt/6.9.3/mingw_64
```

---

## 3. 构建与运行

### Linux / macOS

```bash
cd client

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

./build/cloudvault-client
```

### Windows

Windows 上 **必须**用 `-DCMAKE_PREFIX_PATH` 指向 Qt 的安装前缀（`CMAKE_PREFIX_PATH` 指向
`<Qt>/<版本>/<工具链>` 这一层，**不要**指到 `Qt/` 根目录），且**工具链要与 Qt 套件匹配**：
MinGW 版 Qt 配 MinGW 编译器和 `-G "MinGW Makefiles"`，MSVC 版 Qt 配 Visual Studio 生成器。

> **Linux 编译出的二进制不能拿到 Windows 运行。** Linux 产物是 ELF、链接 Linux 版 Qt 的
> `libQt6*.so`，Windows 产物是 PE、链接 `Qt6*.dll`，可执行文件格式与动态库格式都不同。
> 所谓"双平台兼容"指的是**同一份源码在两个平台各自编译一次**；本项目源码本身不含任何
> 平台相关代码（无 `unistd.h`、`sys/*`、`popen` 等），不需要为 Windows 维护单独分支。
>
> 客户端的 `CMakeLists.txt` 已处理两处平台差异：源码按 UTF-8 编译（MSVC 加 `/utf-8`），
> 以及 Windows 下把目标标记为 GUI 程序（`WIN32_EXECUTABLE TRUE`）——**不加这一条，exe 会是
> Console 子系统，双击运行时除了主窗口还会多弹一个黑色控制台窗口**。

```bat
rem 方式一：MinGW（Qt 自带工具链），无需安装 Visual Studio
cd client
cmake -S . -B build -G "MinGW Makefiles" ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_PREFIX_PATH=D:/Qt/6.9.3/mingw_64 ^
  -DCMAKE_C_COMPILER=D:/Qt/Tools/mingw1310_64/bin/gcc.exe ^
  -DCMAKE_CXX_COMPILER=D:/Qt/Tools/mingw1310_64/bin/g++.exe ^
  -DCMAKE_MAKE_PROGRAM=D:/Qt/Tools/mingw1310_64/bin/mingw32-make.exe
cmake --build build -j

rem 方式二：MSVC（需 Visual Studio 2022，并在"x64 Native Tools"命令行里执行）
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_PREFIX_PATH=D:/Qt/6.9.3/msvc2022_64
cmake --build build --config Release -j

build\cloudvault-client.exe        rem 输出位置，MinGW 在 build\ 下

rem 方式一额外一步：把 Qt 运行时 DLL 部署到 exe 同目录，之后双击 exe 即可运行
cmake --build build --target deploy

rem 不想部署 DLL 的话，运行时把 Qt 的 bin 临时加进 PATH 也可以
set PATH=D:\Qt\6.9.3\mingw_64\bin;%PATH%
build\cloudvault-client.exe
```

> 把 `6.9.3` 换成你机器上实际的 Qt 版本号（例如 `6.9.1`），把盘符/路径换成你的安装位置。
> 构建前用 `cmake --version` 与 `qmake -query QT_VERSION`（或 Qt Creator 的 Kits 页面）核对版本。
> 若 CMake 报 `Could not find a package configuration file provided by "Qt6"`，
> 基本就是 `CMAKE_PREFIX_PATH` 写错或指到了 Qt 根目录，按上面的层数改正即可。
> 若配置阶段卡在 `Check for working CXX compiler` 或 CMakeCache 出现
> `CMAKE_MAKE_PROGRAM-NOTFOUND`，是 PATH 里没有 MinGW（不是找不到 Qt）：把 MinGW 的 `bin`
> 加进 PATH，或像方式一那样显式传 `CMAKE_C/CXX_COMPILER` 与 `CMAKE_MAKE_PROGRAM` 绝对路径。

---

## 4. 界面与验证流程

### 界面

- **顶部**：服务器地址输入框（默认 `http://127.0.0.1:8080`）+ 七个按钮 +
  第二行的 **进度条与进度文案**
- **中部**：由一条可拖拽的分隔条分成左右两栏
  - **左**：文件列表，列为 `ID / 名称 / 大小 / 时间 / Hash 前 8 位 / 是否秒传`
  - **右**：**预览侧边栏**（详见第 4.1 节），选中一行即预览该文件内容
- **底部**：日志区，逐条打印 `方法 + URL`、HTTP 状态码、耗时(ms) 与响应原文
  （单条响应超过 4096 字节会截断，避免 6MB 文件刷爆日志）

七个按钮：

| 按钮 | 接口 / 说明 |
|---|---|
| 健康检查 | `GET /healthz` |
| 选择文件并上传 | `POST /api/v1/files`（**整文件**上传，保留原有功能） |
| 列出文件 | `GET /api/v1/files` |
| 下载选中文件 | `GET /api/v1/files/:id/content`（**整文件**下载，保留原有功能） |
| **分块上传（断点续传）** | `init -> PUT chunk* -> complete`，见第 4.3 节 |
| **取消上传** | `DELETE /api/v1/uploads/:id`，中断当前分块会话 |
| **分块下载（断点续传）** | `GET /api/v1/files/:id/content` 带 `Range`，见第 4.4 节 |
| 清空日志 | 清空日志区 |

请求进行中会自动禁用网络按钮并把鼠标指针置为忙碌态，响应回来后恢复，避免重复点击。

顶端第二行是进度条 + 文案：分块上传时显示 `已传/总块数（百分比） upload_id 命中续传`，
分块下载时显示 `已下载/总大小（百分比）`。

### 4.1 列表的「时间」列

`GET /api/v1/files` 返回的 `created_at` 是 **Unix 纪元毫秒**（见 `server/src/meta/file_repository.cpp`
的 `nowMillis`，由 `server/src/app/main.cpp` 的 `fileToJson` 原样透传）。客户端在 `formatTime`
入口做**毫秒 → 秒归一化**（数值 > 1e11 即视为毫秒量级，除以 1000），再按
`QDateTime::fromSecsSinceEpoch(...).toLocalTime()` 转成**本地时间**，显示格式为 `yyyy-MM-dd HH:mm:ss`。

字段为 `0`、缺失或数值非法时一律显示 `-`，**不会**显示 `1970-01-01` 这种误导性时间。
上传接口按契约不返回 `created_at`，刚上传的行用**本地当前时间**兜底（文件确实是刚刚传上去的）。

### 4.2 右侧预览侧边栏

左侧列表与右侧预览之间是一条 `QSplitter` 分隔条，可自由拖拽调整宽度；预览区默认可见。
在列表里**选中一行**，客户端会异步发 `GET /api/v1/files/{id}/content` 拉取内容并渲染：

| 情况 | 表现 |
|---|---|
| 加载中 | 预览区显示「正在加载预览…」，同时沿用现有模式禁用网络按钮、显示忙碌指针 |
| 空文件（0 字节） | 显示「（空文件）」 |
| 纯文本 | 直接显示文本内容（等宽字体、不自动折行） |
| 二进制 | 提示「二进制文件」并给出**前 1 KiB（1024 字节）的十六进制转储**（偏移 + hex + ASCII） |
| 超过 64 KiB | 只渲染前 64 KiB，末尾注明「（共 X 字节，仅预览前 64.00 KB）」 |
| 超过 16 MiB | **不自动拉取**，提示改用【下载选中文件】；确认要点【仍要预览（可能很慢）】才发起请求 |
| HTTP 非 200 / 网络错误 | 预览区显示状态码与错误信息，不静默、不崩溃 |

**二进制判定规则**：扫描前 8 KiB，出现 `\0` 字节，或整段内容不是合法 UTF-8（用
`QString::fromUtf8()` 往返比对判定）→ 视为二进制。二进制不会硬当文本显示，避免乱码刷屏。
超过 64 KiB 时**先把切点回退到 UTF-8 字符边界**再判定，否则截断出来的半个汉字会让纯文本
被误判成二进制。

**刷新列表 / 上传完成不会白拉预览**：插入行时屏蔽列表的选中变化信号，只有用户主动点选某一行
才发预览请求；刷新结束若已无选中行，预览区会同步清空。

**为什么有 16 MiB 保护**：服务端的 `/content` 接口**不支持 Range 请求**，预览等于把整个文件
拉回内存。所以体积超过 16 MiB 时默认不自动拉，避免一次点选就吃掉十几 MB 流量与内存。
真要看就点【仍要预览】，或用【下载选中文件】存到本地慢慢看。

**切换选中行**时会先 `abort()` 上一个未完成的预览请求（并 `deleteLater`），
被取消的请求不会覆盖新结果，快速点选不会出现内容错乱。

### 4.3 分块上传（断点续传）

点【分块上传（断点续传）】选文件后，客户端按以下流程工作，**整文件不会一次性读进内存**：

1. **流式计算整文件 SHA-256**：用 `QCryptographicHash::Sha256` 配合 1 MiB 缓冲分块喂入，
   同时按 `chunk_size = 5 MiB` 切出每个分块的「偏移 / 长度」表。
2. **本地 manifest 持久化**（见第 4.3.1 节）：读取 `AppDataLocation/cloudvault/upload_manifest.json`，
   仅当 `size + mtime + full_hash` 三者与当前文件**全部一致**才命中续传；任一不一致（源文件被改）即丢弃，从头传。
3. **复用 / 新建会话**：
   - 命中续传 → 先 `GET /api/v1/uploads/:id` 确认 `upload_id` 仍有效；服务端返回 `404` 则丢弃本地进度、改走 `init`。
   - 否则 `POST /api/v1/uploads/init`，body `{name,size,chunk_size,hash}`。
4. **差集上传**：把 `init` 返回的 `uploaded[]` 与本地 manifest 已传集合求并，得到「已确认分块」，
   **跳过它们**，只 `PUT` 缺失分块（按 `X-Chunk-SHA256` 头带分块哈希）；**2~4 并发**（上限 3）。
5. **每收到一个分块 ACK（HTTP 200）立即把已传集合写回 manifest**（保证程序退出不丢进度），再推进下一个。
6. 全部传完 → `POST /api/v1/uploads/:id/complete` → 列表新增一行；成功后删除 manifest。

**失败与重试**：单块失败按 **指数退避 0.5 / 1 / 2 秒** 重试，最多 3 次；耗尽则中断会话并**保留 `upload_id`
与已传分块**，重选同一文件即可从断点继续（`init` 会复用同 hash 会话返回 `uploaded`）。

**错误分支**：`complete` 返回 `409 {missing:[...]}` → 补传缺失分块；`422 {invalid:[...]}`（整文件哈希不符）
→ 只重传 `invalid` 列出的坏分块；若 `invalid` 为空（各分块单检都通过、仅整体哈希不符，通常是声明的
hash 有误）→ 终止重传并保留会话，避免死循环；`init` 若返回 `done:true`（服务端已有相同内容）
→ 视为秒传命中，`file_id` 入列表。

**空文件 / 单分块**：0 字节文件 `total_chunks=0`，跳过上传直接 `complete`；单分块文件只发 1 个分块。
**取消**：点【取消上传】→ `DELETE /api/v1/uploads/:id` 并中断会话，进度保留供续传。

#### 4.3.1 manifest 结构

`AppDataLocation/cloudvault/upload_manifest.json`，键为文件绝对路径，值为：

```json
{
  "<abs_path>": {
    "abs_path": "...", "name": "...", "size": 12345, "mtime": 1700000000000,
    "full_hash": "<整文件 sha256>", "upload_id": 1, "chunk_size": 5242880,
    "uploaded_seqs": [0, 1]
  }
}
```

#### 4.3.2 续传原理小结

> init 复用同 hash 会话 → 返回 `uploaded` 列表 → 客户端取「服务端已传 ∪ 本地 manifest 已传」的差集
> 只传缺失分块 → 每 ACK 落盘 manifest。程序退出 / 网络中断后，重选同文件只要 size+mtime+hash 不变
> 即可从断点继续；源文件被改则 manifest 失效，自动从头传。

### 4.4 分块下载（断点续传）

点【分块下载（断点续传）】选列表中的一行并指定保存路径后：

- 若已存在 `<保存路径>.part`，以它的**当前大小作为 offset**，发 `GET /content` 带 `Range: bytes=<offset>-`；
  服务端返回 `206 + Content-Range`，客户端 `seek(offset)` 后**追加**写入 `.part`。
- 若服务端不支持 Range（返回 `200` 全文），则直接整文件写入最终路径并删除 `.part`。
- 每收到一段就更新进度条；`offset >= total`（取自 `Content-Range` 的 total）时校验大小，
  把 `.part` 重命名为正式文件；大小不符会告警。
- **中断恢复**：程序退出或网络断开后，再次对同文件点【分块下载】会复用 `.part`，从断点继续。
- **边界**：服务文件被删 → `404` 下载失败；服务文件被改（size 变化）→ 以最新 `Content-Range` 的 total 为准继续。



```bash
# 1) 启动服务端（另一个终端）
mkdir -p /tmp/cvdata
../server/build/cloudvault-server --data-dir=/tmp/cvdata --port=8080

# 2) 启动客户端
./build/cloudvault-client
```

按顺序点：

| 步骤 | 操作 | 期望结果 |
|---|---|---|
| 1 | 【健康检查】 | `HTTP 200`，响应 `{"status":"ok","version":"0.5.0","data_dir":"..."}` |
| 2 | 【选择文件并上传】 选 `testdata/hello.txt` | `HTTP 201`，返回 `id`；`"instant":false`；列表新增一行 |
| 3 | 【选择文件并上传】 再传 `testdata/dup-a.txt` | `HTTP 201`，`"instant":false` |
| 4 | 【选择文件并上传】 再传 `testdata/dup-b.txt` | `HTTP 201`，**`"instant":true`**（秒传命中） |
| 5 | 【选择文件并上传】 选 `testdata/中文文件名测试.txt` | 列表里的名称与原始中文文件名完全一致 |
| 6 | 【选择文件并上传】 选 `testdata/empty.txt` | `HTTP 201`，`"size":0` |
| 7 | 【列出文件】 | `HTTP 200`，`{"total":N,"items":[...]}`，表格全量刷新 |
| 8 | 选中 `dup-a.txt` 那一行，【下载选中文件】 | 保存到本地后与源文件逐字节一致（`cmp` / `fc /b`） |
| 9 | 生成大文件后重复步骤 2、8 | `gen_big.sh` 生成约 6MiB，服务端 5MB 分块应返回 `"chunks":2` |
| 10 | 选中 `hello.txt` 那一行 | 右侧预览区顶部显示「大小 / 时间」，正文显示文件内容 |
| 11 | 选中 `empty.txt` 那一行 | 预览区显示「（空文件）」 |
| 12 | 快速连续点选不同行 | 预览区最终显示最后一次选中的文件内容（旧请求已被中止） |

> 说明：**列表接口不返回 `instant` 字段**（见 `main.cpp` 的 `fileToJson`），
> 所以"是否秒传"一列取自本客户端本地记录的上传结果；
> 不是由本客户端上传的文件显示为 `-`，这是预期行为，不是 bug。

### 客户端请求的接口契约

| 方法 | 路径 | 请求 | 响应 |
|---|---|---|---|
| GET | `/healthz` | — | `{"status","version","data_dir"}` |
| POST | `/api/v1/files` | body = 文件原始字节；请求头 `X-CV-Name` = 文件名百分号编码 | `201 {"id","name","size","hash","chunks","instant"}` |
| GET | `/api/v1/files` | — | `{"total":N,"items":[{id,name,size,hash,chunks,created_at}]}` |
| GET | `/api/v1/files/{id}/content` | 可选 `Range: bytes=start-` | `200` 全文 / `206 + Content-Range`，`application/octet-stream` |
| POST | `/api/v1/uploads/init` | JSON `{name,size,chunk_size,hash}` | `200 {upload_id,name,size,chunk_size,hash,uploaded:[seq],received_bytes}`（秒传则带 `done:true`+`file_id`） |
| GET | `/api/v1/uploads/:id` | — | `200 {upload_id,size,chunk_size,uploaded:[...],received_bytes,status}`（失效 404） |
| PUT | `/api/v1/uploads/:id/chunk/:seq` | body = 分块字节；头 `X-Chunk-SHA256` = 分块 sha256 | `200 {seq,received_bytes}`（幂等） |
| POST | `/api/v1/uploads/:id/complete` | — | `200 {file_id,...}`；缺块 `409 {missing:[...]}`；哈希不符 `422 {invalid:[...]}` |
| DELETE | `/api/v1/uploads/:id` | — | `204`（取消会话） |

上传时文件名用 `QUrl::toPercentEncoding(name)` 编码后放进 `X-CV-Name`，
服务端 `util::urlDecode` 还原——中文文件名走这条路径。
分块上传的整文件哈希（`hash`）与每个分块哈希（`X-Chunk-SHA256`）均由客户端用
`QCryptographicHash::Sha256` 计算，分块按 `chunk_size = 5 MiB` 切分。

---

## 5. 测试数据

`testdata/` 下的每个文件对应一个验证点：

| 文件 | 大小 | 验证什么 |
|---|---|---|
| `hello.txt` | 161 B | 最小上传样例；内容含中文，验证 body 原样透传 + 下载回读一致 |
| `中文文件名测试.txt` | 244 B | 中文文件名的百分号编码 / 解码；列表返回的 name 应与原名一致 |
| `dup-a.txt` | 265 B | 秒传基准文件，首次上传 `instant:false` |
| `dup-b.txt` | 265 B | 与 `dup-a.txt` 内容完全相同（SHA-256 同为 `cb7b38d4af761572…`），第二次上传应 `instant:true` |
| `empty.txt` | 0 B | 0 字节边界用例：上传 `size:0`，下载应得到空文件 |
| `gen_big.sh` | — | Linux/Git Bash 下生成 `big.txt`（6291456 B = 6 MiB），服务端 5MB 分块 → `chunks:2`，验证跨分块上传与重组下载 |
| `gen_big.bat` | — | Windows cmd 下生成 `big.txt`（59552 行，实际 6301406 B ≈ 6.01 MiB；内容为纯 ASCII，避免 cmd 代码页把中文写乱码）。与 `gen_big.sh` 的 6291456 B 相差 +9950 B（+0.16%），同样 >5MB 分块阈值 → `chunks:2` |

生成大文件：

```bash
./testdata/gen_big.sh                        # 生成 client/testdata/big.txt
./testdata/gen_big.sh /tmp/big.txt           # 指定输出路径
```

```bat
testdata\gen_big.bat
testdata\gen_big.bat D:\tmp\big.txt
```

`big.txt` 体积较大，已加入 `testdata/.gitignore`，**不要提交到仓库**。

#### 续传验证

- **分块上传断点续传**：选 `big.txt` 点【分块上传（断点续传）】，传输中途点【取消上传】，
  再选同一文件重传。只要 `size + mtime + 哈希` 不变，会跳过已传分块、从断点继续；
  程序退出再启动后重选同文件同理（manifest 持久化在 `AppDataLocation/cloudvault/`）。
- **分块下载断点续传**：用 `testdata/resume_helper.sh` 先把 `big.txt` 的前 N 字节写成
  `<输出>.part`，再在客户端点【分块下载（断点续传）】选该输出路径，客户端从 offset=N 续拉剩余字节；
  或直接传大文件时点【分块下载】，中途关闭客户端再启动重传，复用 `.part` 即可续传。

```bash
./testdata/resume_helper.sh big.txt 1048576 big_downloaded.txt
# 然后客户端【分块下载（断点续传）】选 big_downloaded.txt -> 从 1 MiB 断点续传
```

---

## 6. 已知边界

- 上传走的是"整文件 body"，6MB 大文件会一次性读入内存；服务端单文件上限 256MB。
- 预览同样是"整文件拉取"（服务端 `/content` 不支持 Range），因此有 64 KiB 渲染上限与
  16 MiB 自动预览保护；超过 16 MiB 需要显式点【仍要预览】。
- 服务端每连接只处理一个请求后关闭（`Connection: close`）；本客户端每次都是独立请求，不受影响。
- 服务端目标平台是 Linux/POSIX，Windows 上无法编译 `net` 模块；
  客户端是跨平台的，可以跑在 Windows 上连远程的 Linux 服务端。
