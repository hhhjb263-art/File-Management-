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
│   ├── MainWindow.cpp             # 界面 + 网络请求
│   ├── FileTreeDialog.h
│   └── FileTreeDialog.cpp         # 远端文件树选择对话框（合成 /api/v1/dirs + /api/v1/files 树）
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

- **顶部**：服务器地址输入框（默认 `http://172.20.32.231:8080`；服务端开 HTTPS 时填
  `https://<IP>:<tls端口>`）+ 七个按钮 +
  第二行的 **进度条 + 进度文案 + ✓/✗ 结果标识 + 服务器剩余空间**
  （空间来自 `GET /api/v1/storage`，启动即查、每 60 秒自动刷新、上传/删除后立即刷新；
  低于 10% 或不足 512 MiB 时标红）
- **中部**：由一条可拖拽的分隔条分成左右两栏
  - **左**：文件列表，列为 `ID / 名称 / 大小 / 时间 / Hash 前 8 位 / 是否秒传`（**支持多选**）
  - **右**：**预览侧边栏**（详见第 4.1 节），选中一行即预览该文件内容
- **底部**：日志区，逐条打印 `方法 + URL`、HTTP 状态码、耗时(ms) 与响应原文
  （文本超过 4096 字符只显示前 4096 字符；二进制响应只显示大小 + 前 48 字节十六进制摘要）

七个按钮：

| 按钮 | 接口 / 说明 |
|---|---|
| 健康检查 | `GET /healthz` |
| **选择文件并上传（可多选）** | `POST /api/v1/files`（**整文件**、**多选依次上传**；先弹**文件树对话框**自由选上传位置，可在其中【新建文件夹】，头 `X-CV-Dir`） |
| 列出文件 | `GET /api/v1/files` |
| **下载选中文件（可多选）** | `GET /api/v1/files/:id/content`（**多选顺序下载**：选 1 个用"另存为"选文件名，选多个则选一个保存目录逐个落盘） |
| **分块上传（断点续传）** | `init -> PUT chunk* -> complete`，见第 4.3 节（init body 带 `dir`，目录由文件树选） |
| **取消上传** | `DELETE /api/v1/uploads/:id`，中断当前分块会话 |
| **分块下载（断点续传）** | `GET /api/v1/files/:id/content` 带 `Range`，见第 4.4 节 |
| 清空日志 | 清空日志区 |

> 变更（v0.7）：已**移除**【创建目录】与【按路径下载】两个按钮 —— 建目录改由上传流程内的文件树对话框【新建文件夹】完成，下载统一走列表多选。服务端 `/api/v1/dirs`、`/api/v1/download` 接口仍保留（供其他客户端/脚本使用）。

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

### 4.5 上传位置选择 + 多选上传/下载 + 结果标识

**上传位置自由选择**：点【选择文件并上传（可多选）】→ 先选本地文件（可多选）→ 弹
**文件树对话框**（标题「选择上传位置（可在其中新建文件夹）」），在其中浏览目录、点
【新建文件夹】远程建目录、最后【确定】选定落点。选中的相对路径通过请求头 `X-CV-Dir`
传给服务端（根目录 = 空串）。分块上传的目录选择走同一对话框（写入 `init` body 的 `dir`）。

**多选上传**：选中的多个文件按**队列顺序**逐个 `POST /api/v1/files`（每个 ≤ 256MB），
进度与结果实时显示；单个失败不影响后续文件。

**多选下载**：文件列表已改为 `ExtendedSelection`，用 Ctrl / Shift 多选后点
【下载选中文件（可多选）】——选 1 个仍弹「另存为」选文件名；选多个则弹目录选择框，
逐个下载到该目录（同名文件覆盖）。

**✓/✗ 简化结果标识**：顶部第二行新增结果标签。批量任务进行中显示 `… 上传中 3/5`，
结束后显示 **绿色 ✓ 上传成功 5 个** 或 **红色 ✗ 成功 3 / 失败 2**，不必翻日志即可判断成败。
日志内每条成功/失败行也分别以 `✓` / `✗` 开头，便于快速扫读。

**服务端目录模型**：允许目录树的物理根由配置显式给出（默认 `<dataDir>/files`，可用
`--files-root` / `CV_FILES_ROOT` / 配置文件 `files_root` 指定；**不随服务端启动目录变化**）；
目录元数据存在 `dir_node` 表（DB 为真相源），文件通过硬链接镜像到 `<根>/<dir>/<name>`。
用户路径与磁盘路径**从不直接拼接**——先经 `core/path_util.h` 的 `sanitizeRelPath`
清洗，再按 `(dir, name)` 查库。

**边界规则（两端同规则，服务端为最终裁决）**：
- 拒绝 `.` / `..` 路径段、绝对路径（`/` 开头）、盘符 / 冒号、反斜杠
- 拒绝非法字符 `< > : " | ? *` 与控制字符、空路径段（`a//b`）、`CON/NUL` 等保留名
- 目录创建时逐级检查符号链接：任何一级是符号链接 → **403** 拒绝；
  创建后做规范化包含校验，解析出的真实路径逃出允许根 → **403**
- 非法字符 / 超长 → **400**；越界（穿越 / 绝对路径 / 符号链接）→ **403**（明确报错）
- 客户端在发请求前做同样的预检（`validRelPathInput`），快速给出提示；这只是
  用户体验层的双保险，**不构成安全边界**

> 说明：**边界只限定在配置的允许根内**——客户端可在根内任意建目录/上传/下载，
> 但**不能**写到根之外的任意服务器路径（这是刻意的安全设计，避免上传接口变成写任意文件）。

### 4.6 远端文件树选择对话框（`FileTreeDialog`）

`FileTreeDialog`（`QDialog` + `QTreeWidget`）用于**选择上传位置（可新建文件夹）**与
分块上传选目录。对话框**独立持有 `QNetworkAccessManager`**，与服务端交互不进 MainWindow
的日志区。按钮为【新建文件夹】（`allowCreateDir` 时出现）+【全部展开】【全部折叠】【刷新】
+【确定】【取消】——**【确定】在所有模式下都可用**（选中合法节点后才可点），
【取消】放弃选择。

**数据来源（合成树）**：打开即**并发**拉取两个已有接口，在客户端合成目录树：
- `GET /api/v1/dirs` → `{"total":N,"items":["docs","docs/backup"]}`（扁平规范化相对路径，不含根）
- `GET /api/v1/files` → `{"total":N,"items":[{id,name,dir,size,hash,chunks,created_at}]}`（`dir=""` 为根）

根节点固定为 **`/（根目录）`**（`path = ""`）。`dirs` 只列了 `a/b` 而没有 `a` 时，**自动补出 `a`**
（按 `/` 拆分逐级建节点）；文件按其 `dir` 挂到对应目录节点下（`dir=""` 直接挂根）。

**列**：`名称 | 类型 | 大小 | 修改时间`；文件夹显示「目录」、文件显示「文件」；大小用 `formatSize()`、
时间用 `formatTime(created_at)`（毫秒归一化，与服务端列表一致）；用 `QStyle` 标准图标区分文件夹/文件。

**展开 / 折叠 / 刷新**：
- 默认展开**根 + 一级目录**；仅含子节点的文件夹才显示展开箭头。
- 【全部展开】【全部折叠】一键操作；**双击目录**切换其展开状态。
- 【刷新】会**重新拉取两个列表并重建树**，重建后按 `QSet<QString>` 记录的已展开路径集合
  **恢复展开状态**（展开/折叠事件实时维护该集合，所以用户手动展开过的节点刷新后仍在）。

**单选（不做多选）**：整棵树为 `QAbstractItemView::SingleSelection` + `SelectRows`。
下游语义是"单个文件 / 单个目录"，批量选择需要新接口，故刻意不做多选；README 此处明确说明原因。

**选中反馈**：底部状态标签实时显示
`已选择：docs/backup/note.txt（文件，1.2 KB）` 或 `已选择目录：docs/backup`；
根目录显示 `已选择目录：/（根目录）`；选错类型时提示原因。

**确定按钮按模式校验**：
- `Mode::SelectFile` 必须选中**文件**节点（否则【确定】置灰 + 提示）。
- `Mode::SelectDir` 必须选中**目录**节点（**含根目录**，返回 `""`；选文件则置灰）。

**结果接口**：`exec()` 返回 `Accepted` 时调用方才取值：
- `selectedPath()`：文件 = `dir/name`、目录 = `dir`、根 = `""`；
- `isFile()`：当前是否选中文件；`mode()`：当前模式。
返回 `Rejected` 调用方不做任何动作（取消即不发起请求）。

**加载失败 / 空树**：状态标签显示错误原文（区分网络错误与 HTTP 码），提供【重试】；
两个列表都成功但都为空时显示「（服务器上没有已记录的目录/文件）」。

**创建目录（对话框内新建文件夹）**：`onCreateDir()` 弹 `FileTreeDialog`
（`SelectDir` 模式 + `allowCreateDir=true`），该模式下以**【关闭】代替【确定】**并多出
**【新建文件夹】**按钮。点击后输入**单段**文件夹名 → 复用 `MainWindow::validRelPathInput`
校验该单段（与服务端 `sanitizeRelPath` 同规则）→ 拼成 `父/名`（父为空即 `名`）→
对话框用自身 `QNetworkAccessManager` `POST /api/v1/dirs {path}`（等价原 `cvStep="mkdir"`）→
成功后**自动刷新树并选中新目录**；失败弹窗提示（重名走 `200` 幂等、非法名被客户端预检拦截、
越界 403）。

**结果如何应用到消费点**（v0.7 起只剩两个入口）：
- **【选择文件并上传（可多选）】**：弹 `SelectDir`（`allowCreateDir=true`，标题「选择上传位置」），
  取 `selectedPath()` 作目标目录（根 = `""`）；小文件整传写请求头 `X-CV-Dir`（百分号编码），
  > 8 MiB 的大文件自动转分块上传、写入 `init` body 的 `dir`。
- **【分块上传（断点续传）】**：同一对话框选目标目录，写入 `init` body 的 `dir`。

两个入口都把上次的 `m_lastDir` 作为 `initialPath` 传入，便于默认选中/展开；选完之后用结果回写
`m_lastDir`。

### 4.7 大文件传输（流式 / 分块，内存与文件大小解耦）

**多线程（v0.9）**：所有耗时 IO/哈希都跑在专用线程池 `m_ioPool`（3 线程）里，**UI 线程只做界面**——
① 整文件扫描 + SHA-256（`scanFileForChunks`，大文件要几十秒也只显示"正在扫描"）；
② 每个分块的 `seek + read` 与**分块 SHA-256**（`launchChunkAsync`）；
③ 上传/下载的队列推进、网络收发仍在主线程（QNAM 本身异步）。
扫描/读取期间相关按钮禁用并有防重入保护；批量上传遇到"正在扫描"会自动排队重试（不记失败）。

- **上传 > 8 MiB 自动转分块**：`prepareChunkPlan` 用 1 MiB 缓冲流式算哈希 + 建分块表（不载入文件），
  逐块 `seek+read` 5 MiB 发送；内存峰值 ≈ 1 个分块。
- **下载一律 Range 分段 + 4 路并发窗口**：每次请求 `bytes=offset-(offset+8MiB-1)`（**有界**），
  同时保持 **4 个在途段**（`kDownloadConcurrency=4`，对齐服务端 4 工作线程）；乱序到达的段先缓存，
  按序追加写 `<目标>.part`（写盘串行化防乱序），中断后凭 `.part` 大小续传；内存峰值 ≈ 窗口内几段。
  服务端则优先**镜像直读**（顺序读文件树镜像，比分块拼装快得多；日志以 `[镜像直读]`/`[分块拼装]` 区分）。
- **服务端兜底**：整文件上传 > 64 MiB → `413`；无 Range 的整文件下载 > 8 MiB → `409`（提示用 Range），
  确保任一端都不会因为一次大 body / 一次全文回发而吃满内存。

> 完整设计（适用场景、传输流程、内存策略、异常与中断处理、阈值总览）见
> [`docs/大文件传输设计.md`](../docs/大文件传输设计.md)。

### 4.9 HTTPS / 自签名证书（v0.9）

服务端启用 HTTPS 后（`--tls-port=8443 --tls-cert=... --tls-key=...`），客户端地址栏直接填
`https://<IP>:8443` 即可，其余功能完全一致。

- **TLS 证书采用 TOFU（Trust On First Use）指纹固定**，不再「无脑信任任意自签名证书」：
  - 顶部第二行【允许使用自签名证书（首次需确认）】复选框**默认勾选**。勾选后，首次连接某台
    `host:port` 的 HTTPS 服务器时，客户端弹出确认框，展示**服务器地址、证书 SHA-256 指纹
    （`AA:BB:CC:…` 便于人眼比对）、使用者 / 签发者 / 有效期**，点「信任」即把该指纹写入本地
    （`QSettings` 键 `pinnedFingerprint/<host>:<port>`）并继续；点「取消」本次不信任，连接失败。
  - 之后同主机指纹**一致** → 静默放行；指纹**不一致** → 弹警告
    「⚠ 证书指纹不匹配（可能存在中间人攻击）」，**绝不继续**，需点同行的【证书…】按钮清除该主机
    信任后才能重连（不提供「仍然继续」之类的逃生通道）。
  - 取消勾选该复选框 → 完全不介入，走 Qt 标准证书链校验（自签名证书会被拒，配合正式 CA 证书使用）。
  - 同行【证书…】按钮：只读展示当前服务器（地址栏里的 `host:port`）已固定的指纹（未固定显示
    「未固定」），并提供【清除该主机信任】按钮，便于自助恢复（也解决指纹不匹配后的死锁）。
- 服务端生成自签名证书：`bash server/testdata/gen_cert.sh [CN] [服务器IP]`（含 SAN，10 年有效）。

### 4.10 访问令牌（鉴权）

- 顶部第一行【访问令牌】输入框填写服务端下发的 Token（密码框显示，**可留空**）；留空表示
  不带 `Authorization` 头，服务端未启用鉴权时行为不变。
- 编辑完成后**失焦即持久化**到 `QSettings` 键 `authToken`，下次启动自动回填。
- 所有出网请求（上传 / 列表 / 下载 / 预览 / 分块上传下载 / 文件树对话框）都会自动在请求头带上
  `Authorization: Bearer <token>`；Token 自动 `trim` 前后空格，为空时不加该头。

### 4.8 文件列表右键菜单、排序与同名检测（v0.8）

**右键菜单**（在文件列表上右键，右键会先选中该行）：

| 菜单项 | 行为 |
|---|---|
| 新建文件 | `POST /api/v1/files/new {dir,name}` 在**右键行所在目录**新建空文件；同名 → 弹「文件已存在」 |
| 重命名「xxx」 | `POST /api/v1/files/:id/rename {name}`；同名 → 弹「名称冲突」，不做改动 |
| 删除「xxx」 | 二次确认后 `DELETE /api/v1/files/:id`（软删除 + 移除镜像文件） |
| 下载 | 复用多选下载（右键已选中该行） |
| 打开文件夹 | 打开本地保存目录：若选中行已下载过，用资源管理器**定位并高亮该文件**；否则打开最近一次下载目录（从未下载则打开系统"下载"文件夹） |
| 上传到此目录 | 选本地文件（可多选）上传到该行所在目录；**同名 → 询问是否覆盖** |
| 排序 ▸ | 按名称 / 按大小 / 按时间，及升序 / 降序 |

**排序**：点击表头也可排序（点「大小」「时间」列按数值，其他列按名称）；升/降在同列重复点击切换。
排序只重排客户端数据模型，不请求服务端。

**同名检测（两端联动）**：上传 / 新建 / 重命名 都会命中服务端 `409`；客户端弹窗询问
「是否覆盖？」——选覆盖，整文件上传带 `X-CV-Overwrite: 1`、分块上传在 `init` 带
`"overwrite": true` 重发（覆盖成功返回 `200` + `overwritten:true`）；选否即跳过该项，批量上传继续下一个。

**列表未显示目录列**：`dir` 存在每行「名称」单元格的 `Qt::UserRole+1` 里，供「新建文件 / 上传到此目录」使用。

### 客户端请求的接口契约

| 方法 | 路径 | 请求 | 响应 |
|---|---|---|---|
| GET | `/healthz` | — | `{"status","version","data_dir","files_root"}` |
| POST | `/api/v1/files` | body = 文件原始字节；请求头 `X-CV-Name` / `X-CV-Dir` = 百分号编码；覆盖时加 `X-CV-Overwrite: 1` | `201` 新建 / `200` 覆盖（均 `{id,name,dir,size,hash,chunks,instant[,overwritten]}`）；同名未覆盖 → `409 {exists:true,...}` |
| POST | `/api/v1/files/new` | JSON `{dir,name}` | `201 {id,name,dir,size:0,hash}`；同名 → `409 {exists:true,...}`；名称非法 → `400` |
| POST | `/api/v1/files/:id/rename` | JSON `{name}` | `200 {id,name,dir}`；同名 → `409 {exists:true,...}`；与原同名 → `200`（幂等） |
| DELETE | `/api/v1/files/:id` | — | `200 {deleted,file_id,name,dir,freed_bytes,blobs_removed,disk_free_bytes}`（删除并回收 blob 空间；客户端提示"释放 X"并刷新空间显示） |
| GET | `/api/v1/storage` | — | `{data_dir,files_root,free_bytes,total_bytes,upload_safety_factor}` |

> **服务器空间不足**：上传（整文件 / 分块 `init`）会先做空间预检，不足返回 **507**；
> 客户端弹「服务器拒绝上传（507）」并原样展示 `need_bytes`/`free_bytes`；批量上传时只提示一次。
| GET | `/api/v1/files` | — | `{"total":N,"items":[{id,name,dir,size,hash,chunks,created_at}]}` |
| GET | `/api/v1/files/{id}/content` | 可选 `Range: bytes=start-` | `200` 全文 / `206 + Content-Range`，`application/octet-stream` |
| GET | `/api/v1/download` | `?path=dir/name`（百分号编码） | `200` 全文 / `206`（同上）；未记录 `404`；非法 `400`；越界 `403` |
| POST | `/api/v1/dirs` | JSON `{path}` | `201 {path,created:true}` / `200 {path,exists:true}`；非法 `400`；越界/符号链接 `403` |
| GET | `/api/v1/dirs` | — | `{"total":N,"items":[path,...]}` |
| POST | `/api/v1/uploads/init` | JSON `{name,size,chunk_size,hash,dir}` | `200 {upload_id,name,size,chunk_size,hash,uploaded:[seq],received_bytes}`（秒传则带 `done:true`+`file_id`） |
| GET | `/api/v1/uploads/:id` | — | `200 {upload_id,size,chunk_size,uploaded:[...],received_bytes,status}`（失效 404） |
| PUT | `/api/v1/uploads/:id/chunk/:seq` | body = 分块字节；头 `X-Chunk-SHA256` = 分块 sha256 | `200 {seq,received_bytes}`（幂等） |
| POST | `/api/v1/uploads/:id/complete` | — | `200 {file_id,name,dir,size,hash}`；缺块 `409 {missing:[...]}`；哈希不符 `422 {invalid:[...]}` |
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
