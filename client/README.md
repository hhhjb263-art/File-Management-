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

```bat
rem 方式一：MinGW（Qt 自带工具链），无需安装 Visual Studio
cd client
cmake -S . -B build -G "MinGW Makefiles" ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_PREFIX_PATH=D:/Qt/6.9.3/mingw_64 ^
  -DCMAKE_CXX_COMPILER=D:/Qt/Tools/mingw1310_64/bin/g++.exe ^
  -DCMAKE_MAKE_PROGRAM=D:/Qt/Tools/mingw1310_64/bin/mingw32-make.exe
cmake --build build -j

rem 方式二：MSVC（需 Visual Studio 2022，并在"x64 Native Tools"命令行里执行）
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_PREFIX_PATH=D:/Qt/6.9.3/msvc2022_64
cmake --build build --config Release -j

build\cloudvault-client.exe        rem 输出位置，MinGW 在 build\ 下
```

> 把 `6.9.3` 换成你机器上实际的 Qt 版本号（例如 `6.9.1`），把盘符/路径换成你的安装位置。
> 构建前用 `cmake --version` 与 `qmake -query QT_VERSION`（或 Qt Creator 的 Kits 页面）核对版本。
> 若 CMake 报 `Could not find a package configuration file provided by "Qt6"`，
> 基本就是 `CMAKE_PREFIX_PATH` 写错或指到了 Qt 根目录，按上面的层数改正即可。

---

## 4. 界面与验证流程

### 界面

- **顶部**：服务器地址输入框（默认 `http://127.0.0.1:8080`）+ 五个按钮
  【健康检查】【选择文件并上传】【列出文件】【下载选中文件】【清空日志】
- **中部**：文件列表，列为 `ID / 名称 / 大小 / Hash 前 8 位 / 是否秒传`
- **底部**：日志区，逐条打印 `方法 + URL`、HTTP 状态码、耗时(ms) 与响应原文
  （单条响应超过 4096 字节会截断，避免 6MB 文件刷爆日志）

请求进行中会自动禁用四个网络按钮并把鼠标指针置为忙碌态，响应回来后恢复，避免重复点击。

### 验证流程（**先启动服务端，再点按钮**）

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

> 说明：**列表接口不返回 `instant` 字段**（见 `main.cpp` 的 `fileToJson`），
> 所以"是否秒传"一列取自本客户端本地记录的上传结果；
> 不是由本客户端上传的文件显示为 `-`，这是预期行为，不是 bug。

### 客户端请求的接口契约

| 方法 | 路径 | 请求 | 响应 |
|---|---|---|---|
| GET | `/healthz` | — | `{"status","version","data_dir"}` |
| POST | `/api/v1/files` | body = 文件原始字节；请求头 `X-CV-Name` = 文件名百分号编码 | `201 {"id","name","size","hash","chunks","instant"}` |
| GET | `/api/v1/files` | — | `{"total":N,"items":[{id,name,size,hash,chunks,created_at}]}` |
| GET | `/api/v1/files/{id}/content` | — | `application/octet-stream`，原样写入用户选择的保存路径 |

上传时文件名用 `QUrl::toPercentEncoding(name)` 编码后放进 `X-CV-Name`，
服务端 `util::urlDecode` 还原——中文文件名走这条路径。

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

---

## 6. 已知边界

- 上传走的是"整文件 body"，6MB 大文件会一次性读入内存；服务端单文件上限 256MB。
- 服务端每连接只处理一个请求后关闭（`Connection: close`）；本客户端每次都是独立请求，不受影响。
- 服务端目标平台是 Linux/POSIX，Windows 上无法编译 `net` 模块；
  客户端是跨平台的，可以跑在 Windows 上连远程的 Linux 服务端。
