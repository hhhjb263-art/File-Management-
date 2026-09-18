# 云匣 CloudVault · 文档索引

个人私有云网盘：**C++17 服务端（Linux/POSIX + SQLite3，零第三方依赖）+ Qt 6 客户端**。
本目录是「设计与规范类」文档；**代码级说明在各子项目的 README**。

## 一、文档地图

| 文档 | 内容 | 什么时候看 |
|---|---|---|
| [`AC-同步用户故事验收标准15条.md`](AC-同步用户故事验收标准15条.md) | 「文件夹自动双向同步」用户故事的 15 条验收标准（含优先级、覆盖度自检、常见写法错误） | 需要写/评审验收标准时；需求对齐 |
| [`TECH-STACK-技术选型（按优先级）.md`](TECH-STACK-技术选型（按优先级）.md) | 技术栈选型总览（P0/P1/P2 分层）+ ADR 摘要 + 许可合规清单 | 新增组件前查「为什么选它 / 还能换什么」 |
| [`大文件传输设计.md`](大文件传输设计.md) | 流式/分块传输：适用场景、传输流程、内存控制策略、异常与中断处理、阈值总览、已知限制 | 改上传/下载链路或排查内存/续传问题时 |
| [`测试用例-目录与按路径下载.md`](测试用例-目录与按路径下载.md) | 41 条用例（P0/P1/P2）：目录管理、按路径下载、上传指定目录、客户端 UI、分块上传续传回归 | 回归测试、上线前验证、QA 执行 |
| [`../PRD-个人私有云网盘系统.md`](../PRD-个人私有云网盘系统.md) | 产品需求文档（在仓库根目录） | 想了解「产品要做什么」 |

## 二、代码级文档

| 文档 | 内容 |
|---|---|
| [`../server/README.md`](../server/README.md) | 服务端分层结构、Linux 编译命令、运行参数、**全部 HTTP 接口与 curl 验证**、目录树/大文件规则 |
| [`../client/README.md`](../client/README.md) | 客户端按钮与交互、Linux/Windows 编译命令、接口契约表、各功能验证步骤、测试数据 |

## 三、快速上手

```bash
# 1) 服务端（必须在 Linux 上编译运行：POSIX socket + SQLite3）
cd server
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/cloudvault-server --data-dir=/tmp/cvdata --port=8080

# 2) 客户端（Windows / MinGW，详见 client/README.md §3）
cmake -S client -B client/build -G "MinGW Makefiles" \
  -DCMAKE_PREFIX_PATH=D:/Qt/6.9.3/mingw_64 \
  -DCMAKE_C_COMPILER=D:/Qt/Tools/mingw1310_64/bin/gcc.exe \
  -DCMAKE_CXX_COMPILER=D:/Qt/Tools/mingw1310_64/bin/g++.exe \
  -DCMAKE_MAKE_PROGRAM=D:/Qt/Tools/mingw1310_64/bin/mingw32-make.exe
cmake --build client/build -j
```
冒烟脚本：`server/testdata/smoke_chunked.sh`（分块续传）、`server/testdata/smoke_dirs.sh`（目录/按路径下载）。

## 四、建议阅读顺序

1. **新同学**：本文件 → `../PRD-个人私有云网盘系统.md` → `../server/README.md` §1 分层 → `../client/README.md` §4 界面
2. **改服务端**：`../server/README.md` §1/§4 → `大文件传输设计.md`
3. **改客户端**：`../client/README.md` §4 → `大文件传输设计.md`
4. **准备测试**：`测试用例-目录与按路径下载.md` → 两个 smoke 脚本
5. **技术选型讨论**：`TECH-STACK-技术选型（按优先级）.md` §6 ADR

## 五、文档维护约定

- **接口变更**：先改 `../server/README.md` 的接口表，再同步 `../client/README.md` 的契约表，最后动代码注释。
- **阈值/限制变更**（如分块大小、上传上限）：`大文件传输设计.md` §5 阈值总览是**唯一权威表**，改代码时必须同步它。
- **版本标记**：用 `v0.x` 标注行为变更（例如「v0.7 移除【创建目录】【按路径下载】按钮」），便于回溯。
- 文档中引用的路径一律相对本仓库根，避免绝对路径。


---

## 运行依赖部署（Windows，正式客户端）

`bin/` 下的 `CloudVault.exe` 需要 Qt 运行时才能运行。**`bin/` 已在 `.gitignore` 中**（体积约 100 MB，不入库），
因此克隆仓库或重新构建后，请执行一次部署脚本：

```bash
bash scripts/deploy_windows.sh            # 默认 Qt=D:/Qt/6.9.3/mingw_64
bash scripts/deploy_windows.sh <Qt前缀> <MinGW前缀>
```

脚本做四件事（幂等，可重复运行）：
1. `windeployqt --qmldir qml`：收集 Qt DLL、平台插件与 **QML 模块**（`qml/` 下 31 个模块、1300+ 文件；FluentWinUI3 风格必需）
2. **补齐 TLS 后端插件**（`bin/tls/` 三个后端）——`windeployqt` 有时只拷 2 个，而自签名 HTTPS 依赖它
3. **补齐 MinGW 运行库**（`libgcc_s_seh-1.dll` / `libstdc++-6.dll` / `libwinpthread-1.dll`）——跨机器运行必需
4. 用 `objdump` 校验 exe 的**全部直接依赖**都落在 `bin/`

**验证自包含**（把 Qt/MinGW 从 PATH 剔除后仍应启动并驻留）：

```bash
CLEAN=$(echo "$PATH" | tr ':' '\n' | grep -iv "qt\|mingw" | paste -sd: -)
env PATH="$CLEAN" QT_QPA_PLATFORM=offscreen timeout 5 ./bin/CloudVault.exe   # 期望 exit=124
```

完成后的 `bin/` 可**直接双击运行**，或整目录拷到其他 Windows 机器（无需安装 Qt）。

> ⚠️ **TLS 后端说明**：本机 Qt 未附带 OpenSSL 运行库，故 `bin/tls/qopensslbackend.dll` 无法加载，
> Windows 会使用系统 **Schannel** 后端（对自签名证书同样会触发 `sslErrors` → TOFU 确认框正常工作）。
> 若后续要统一为 OpenSSL 行为：从 Qt 官方安装器的 "OpenSSL Toolkit" 取得 `libssl-3-x64.dll` /
> `libcrypto-3-x64.dll` 放进 `bin/`，并在 `src/app/main.cpp` 加 `QSslSocket::setActiveBackend("openssl")`。
