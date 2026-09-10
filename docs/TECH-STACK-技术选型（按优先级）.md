# 技术栈与技术选型文档（按优先级）

| 项目 | 内容 |
|---|---|
| 产品 | 云匣 CloudVault —— 个人私有云网盘 |
| 关联文档 | `PRD-个人私有云网盘系统.md` v0.1 |
| 文档版本 | v1.1 |
| 撰写日期 | 2026-09-08（v1.1 于 2026-09-10 更新：ADR-1 服务端语言 Go → C++17） |
| 关联工程 | 本仓库 Qt 6 / C++17 客户端（File.pro，qmake，模块化 .pri 装配） |
| 目标读者 | 开发、测试、部署维护者 |

---

## 1. 优先级定义

| 级别 | 含义 | 引入版本 | 交付约束 |
|---|---|---|---|
| **P0** | 没有它 MVP 不成立／不可发布 | V0.5 MVP | 选型必须在开工前锁定，中途不换 |
| **P1** | V1.0 正式版必须，MVP 可临时降级实现 | V1.0 | MVP 期预留接口，不引入阻断性耦合 |
| **P2** | 可选增强，允许延后或裁剪 | V1.1 / V2.0+ | 必须有开关，禁止反向污染 P0 代码 |

**选型裁决原则**（冲突时按此顺序）：
1. **部署简单 > 功能强大**：单人维护的产品，运维成本是首要成本。
2. **统一语言心智负担最小化**：已有 Qt/C++ 客户端，服务端选 Go（语法简单、单二进制、无运行时依赖），避免引入第二重型技术栈。
3. **必须跨 Windows / Linux / macOS 三端**：任一选型在三端不可用的直接淘汰。
4. **优先标准库与成熟稳定依赖**：新依赖必须说明理由，且需给出可替换的备选。
5. **许可证安全**：服务端依赖限 Apache-2.0 / MIT / BSD；**禁止**引入 AGPL 依赖（避免私有部署合规风险）；ffmpeg 类 GPL 依赖以外部进程方式调用，不静态链接。

---

## 2. 选型总览（一张表看全部）

| 层 | 组件 | 主选 | 备选 | 优先级 |
|---|---|---|---|---|
| 桌面客户端 | GUI 框架 | **Qt 6.8 LTS（C++17 + QML + Quick Controls）** | Qt 6.5 LTS | P0 |
| 桌面客户端 | 编译标准 | C++17 | C++20（Qt 6.8 支持后评估） | P0 |
| 桌面客户端 | 本地索引 | Qt SQL + SQLite（QSQLITE） | 自研 KV | P0 |
| 桌面客户端 | 文件监听 | QFileSystemWatcher + 平台原生 API 双保险 | 仅 QFileSystemWatcher | P0 |
| 桌面客户端 | 网络 | Qt Network（QNetworkAccessManager / QSslSocket） | libcurl | P0 |
| 桌面客户端 | 并发 | Qt Concurrent + QThreadPool | std::thread 自研 | P0 |
| Web 前端 | 框架 | **Vue 3 + TypeScript + Vite** | React 18 | P0 |
| Web 前端 | UI 库 | Element Plus（中文本地化完善） | Naive UI | P0 |
| Web 前端 | 状态/路由 | Pinia + Vue Router 4 | — | P0 |
| Web 前端 | 文件哈希 | hash-wasm（WASM，浏览器端 SHA-256） | spark-md5（不安全，仅作降级） | P0 |
| 移动端 | 框架 | Flutter 3.x | 原生双端 | P2 |
| 服务端 | 语言 | **C++17（Linux / POSIX）** | Go 1.22 / Rust | P0 |
| 服务端 | HTTP 框架 | **自研 net 模块**（POSIX socket + 线程池，见 server/src/net） | cpp-httplib / QtHttpServer | P0 / P2 |
| 服务端 | WebSocket | 复用 net 模块实现握手与帧解析 | websocketpp | P2 |
| 服务端 | 数据访问 | **sqlite3 C API + Repository 封装**（server/src/meta） | SQLiteCpp / SOCI | P0 |
| 服务端 | 建表迁移 | **meta/schema.h 内嵌 DDL + IF NOT EXISTS** | 独立迁移工具 | P0 / P1 |
| 服务端 | 元数据库 | **SQLite（WAL 模式）** | PostgreSQL 16（≥20 用户） | P0 / P2 |
| 存储 | 内容存储 | 本地文件系统 + SHA-256 内容寻址（5MB 分块） | — | P0 |
| 存储 | 对象存储抽象 | S3 兼容接口层（内部接口预留） | MinIO SDK | P2 |
| 安全 | 密码哈希 | argon2id（libsodium / 自实现；C++ 侧优先复用成熟实现） | bcrypt | P0 |
| 安全 | 会话 | 服务端 session 表 + Token（支持踢下线） | JWT 库 jwt-cpp | P0 |
| 安全 | 传输 | TLS 1.2+；Caddy 自动签发 Let's Encrypt | 自签证书 | P0 / P1 |
| 安全 | 两步验证 | TOTP（pquerna/otp） | — | P2 |
| 媒体处理 | 缩略图 | disintegration/imaging | 服务端不生成，客户端渲染 | P1 |
| 媒体处理 | 视频封面/转码 | ffmpeg（外部进程调用） | — | P1 / P2 |
| 部署 | 容器 | Docker + Docker Compose | 裸机二进制 | P0 |
| 部署 | 反向代理 / HTTPS | Caddy 2（自动 HTTPS） | Nginx + acme.sh | P1 |
| 部署 | Windows 安装包 | NSIS / Inno Setup | WiX | P1 |
| 部署 | Linux 守护 | systemd | supervisor | P1 |
| 工程 | 构建（客户端） | qmake（现状）→ **CMake 迁移** | — | P0 → P1 |
| 工程 | 日志（服务端） | core/logger 自研结构化日志（stdout / 文件） | spdlog | P0 / P2 |
| 工程 | 日志（客户端） | QLoggingCategory + 自研滚动文件 Appender | spdlog | P0 / P1 |
| 工程 | 单元测试（服务端） | 自研轻量断言 + cv_tests 可执行自检 | GoogleTest | P0 |
| 工程 | 单元测试（客户端） | Qt Test Framework | GoogleTest | P0 |
| 工程 | 前端测试 | Vitest + Playwright | Jest | P1 |
| 工程 | 集成测试 | curl 脚本 + 临时数据目录 | Testcontainers | P1 |
| 工程 | CI | GitHub Actions（Linux 构建 + cv_tests） | Gitea Actions（私有部署） | P0 |
| 工程 | 代码规范 | clang-format + clang-tidy + clazy | — | P1 |
| 工程 | 依赖更新 | Dependabot | renovate | P1 |
| 监控 | 指标 | 内置 /metrics（文本格式） + 简易监控页 | Prometheus C++ client | P1 |
| 监控 | 链路追踪 | OpenTelemetry | — | P2 |
| 监控 | 崩溃上报 | Crashpad（客户端） | Breakpad | P2 |

---

## 3. P0 —— MVP 必须落地（V0.5）

> 这一档的选型一旦开工就不再变更，任何替换都需要重新评审。

### 3.1 服务端：C++17 单二进制（Linux / POSIX）

> 已在 `server/` 落地：core / meta / store / net / app 五层，CMake 构建，零第三方依赖（仅 SQLite3）。

| 项 | 决策 | 理由 |
|---|---|---|
| 语言 | **C++17** | 与桌面客户端 Qt 统一语言，团队只需一套心智；Linux 上 g++/clang++ 直接可得；单二进制、无运行时依赖 |
| HTTP 框架 | **自研 net 模块**：POSIX socket + 固定线程池 + 路径参数路由 | 目标是"零第三方依赖"；MVP 每连接单请求后关闭，实现简单且不易出错；后续可换 cpp-httplib 而不影响上层 |
| WebSocket | 模块 6 时在 net 层自行实现握手与帧 | 保持依赖清单干净；必要时引入 websocketpp |
| 数据访问 | **sqlite3 C API + Repository 封装** | 直接可控、无 ORM 隐式行为；配合 `meta/schema.h` 内嵌 DDL，版本升级只需改一处 |
| 建表迁移 | DDL 内嵌 + `IF NOT EXISTS`，启动即建表 | 单机产品不需要重型迁移工具；将来如需版本化迁移再引入 |
| 日志 | 自研 `core/logger` 结构化日志（stdout / 文件） | 零依赖、线程安全、格式统一；等级可按配置切换 |

**关键取舍：** 原方案是 Go（部署体积与并发模型更优），改为 C++ 后部署仍为单二进制（依赖 `libsqlite3`），
换来的是**客户端与服务端同一种语言、同一套调试与工程习惯**，对单人/小团队维护更有利。
代价：HTTP 与 JSON 等基础设施需自己写并维护 —— 这些已封装在 `core` 与 `net` 层，且已有自检覆盖。

### 3.2 元数据库：SQLite（WAL）

| 决策 | 说明 |
|---|---|
| 默认 SQLite | 单实例 ≤20 用户、≤10 万文件场景毫无压力；备份 = 拷一个文件，与产品"可整体拷走"的价值主张一致 |
| 必开 WAL + busy_timeout | 读写并发（同步写入 + 查询）必须靠 WAL，否则写锁会阻塞 Web 端浏览；`server/src/meta/db.cpp` 已设 busy_timeout=5s |
| 驱动：系统 libsqlite3 | C++ 直接用官方 C API（`libsqlite3-dev`）；部署镜像用 debian-slim 即可，不必追求全静态 |
| 抽象层隔离 | 数据访问必须经由 Repository 接口，**禁止**在业务代码里写 SQL 方言，为将来切 PostgreSQL 留退路 |

### 3.3 存储：内容寻址 + 5MB 分块

- 存储布局：`/data/files/<hash前2位>/<hash>`，`/data/meta/cv.db`，回收站靠元数据软删除（不做物理搬移）。
- 分块大小 5MB，SHA-256 做块键 → 天然获得**秒传去重**与**增量同步**能力。
- 一致性顺序：**先落盘内容 → 再提交元数据 → 最后广播事件**；崩溃重启由对账任务修复悬挂状态。

### 3.4 桌面客户端：Qt 6.8 LTS（与现有工程一致）

工程现状已符合要求，本档只需**补齐与锁定**：

| 项 | 现状 | 动作 |
|---|---|---|
| Qt 版本 | Qt 6 + C++17 | 锁定 **6.8 LTS**（长支持、Bugfix 持续到 2028 前后） |
| Qt 模块 | core/gui/qml/quick/quickcontrols2/network/sql/concurrent | 保持；托盘需额外引入 **widgets**（见 P1） |
| 构建系统 | qmake（File.pro + 各模块 .pri） | MVP 沿用；**P1 阶段迁移 CMake**（Qt 官方推荐，Qt 7 计划移除 qmake） |
| UTF-8 与 MSVC 开关 | 已在 config.pri 处理 `/utf-8` | 保持，新增源文件必须同样受约束 |
| 分层依赖 | core/net/sync/transfer/data/controllers/app | 严格遵守，**禁止跨层反向依赖**（已在 File.pro 注释中约定） |

### 3.5 Web 前端：Vue 3 + TS + Vite

- **Element Plus** 提供中文文案与表单/上传组件，覆盖文件库、分享管理、统计页等后台型界面。
- 浏览器端大文件哈希必须用 **hash-wasm**（放进 Web Worker），避免主线程卡死；**不得**使用 MD5 作为内容去重键。
- 上传链路：预上传请求 → 缺失分块清单 → 并发上传 → 提交完成，全部走统一队列组件。

### 3.6 安全基线（P0，无商量余地）

| 要求 | 实现 |
|---|---|
| 密码存储 | argon2id，参数按 OWASP 建议配置，禁止裸哈希/SHA-1/MD5 |
| 提取码存储 | 同 argon2id，服务端**只存哈希**，明文仅在创建时返回一次 |
| 会话 | JWT 承载身份，服务端 session 表记录生效状态 → 支持"踢下线 30s 内停止传输"（AC-15） |
| 登录防护 | 连续 5 次失败锁定该 IP/账号 10 分钟 |
| 越权与路径穿越 | 所有文件操作在 Repository 层强制 `owner_id` 过滤；路径参数先规范化再校验前缀 |
| 传输加密 | 全链路 TLS 1.2+，禁用弱套件 |

### 3.7 CI 与质量门禁（MVP 期最低要求）

- GitHub Actions：Go build + `-race` 单元测试 + Qt 构建冒烟。
- 提交规范：Conventional Commits + SemVer，**禁止直接向主干推送**。
- gofmt / go vet 作为 CI 必过门禁。

---

## 4. P1 —— V1.0 正式版补齐

| 组件 | 用途 | 备注 |
|---|---|---|
| Caddy 2 | 反向代理 + 自动 HTTPS（替代手工配 Nginx + acme.sh） | 显著降低非技术用户部署门槛（G4 目标） |
| disintegration/imaging | 图片缩略图生成（列表/网格视图） | 异步生成 + 缓存，失败降级为类型图标 |
| ffmpeg（外部进程） | 视频封面抽帧；不做转码 | 以子进程调用，注意 GPL 许可证隔离 |
| QLoggingCategory + 文件滚动 | 客户端日志落盘，支持用户一键导出 | 便于远程排障 |
| spdlog（可选） | 若自研 Appender 成本过高则引入 | MIT 许可 |
| Qt Widgets / QSystemTrayIcon | 托盘菜单、开机自启、单实例守护 | 会为包体增加约 2~4MB，需确认可接受 |
| CMake 迁移 | 替代 qmake，统一三端构建与 CTest 集成 | 建议在客户端功能冻结后的空窗期做 |
| Vitest + Playwright | 前端单测与端到端（上传/分享/版本恢复主链路） | 至少覆盖 3 条核心 E2E |
| Testcontainers-go | 服务端集成测试：真实 SQLite + 真实文件系统 | 重点覆盖冲突、续传、回收站还原 |
| golangci-lint / clang-format / clazy | 规范自动检查 + pre-commit | clazy 可捕获 Qt 特有反模式 |
| Prometheus + /metrics | 同步队列长度、错误率、传输速率 | 配一个极简内置监控页即可，不强制 Grafana |
| NSIS / Inno Setup（Win）+ systemd（Linux） | 桌面端安装包与后台服务 | 30 分钟部署目标依赖它 |
| Dependabot | 依赖安全更新提醒 | 特别关注 Go 与 npm 的高危 CVE |

---

## 5. P2 —— V1.1 / V2.0+ 可选

| 组件 | 用途 | 前置条件 |
|---|---|---|
| PostgreSQL 16 | ≥20 用户或高并发写场景 | Repository 抽象层已稳定，迁移用 golang-migrate 双方言 |
| S3 兼容对象存储 | 备份到 MinIO / COS / OSS | 存储层已抽象为 Store 接口 |
| flutter 3.x | 移动端（相册自动备份） | v2.0 启动，双端原生亦可 |
| TOTP（pquerna/otp） | 两步验证 | 登录链路稳定后追加 |
| AES-256-GCM 静态加密 | 落盘文件加密（可选开关） | 密钥派生与主口令方案需单独设计，一旦开启不可逆 |
| SQLite FTS5 / bleve + gse | 全文检索（含中文分词） | 需评估索引耗时与磁盘占用，做成开关 |
| golang.org/x/net/webdav | WebDAV 接口（播放器/OBS/挂载盘） | 认证与配额需与 Web 端复用同一套中间件 |
| OpenTelemetry | 分布式追踪 | 单机场景收益有限，优先级最低 |
| Crashpad | 客户端崩溃上报 | 需用户授权开关与隐私声明 |

---

## 6. 关键决策记录（ADR 摘要）

| # | 决策 | 结论 | 被否决方案及原因 |
|---|---|---|---|
| ADR-1 | 服务端语言 | **C++17（Linux/POSIX）**（v1.1 变更，原为 Go 1.22） | Go（语言栈分裂，客户端 Qt/C++ 需维护两套心智）；Node.js（部署需运行时）；Rust（开发效率过慢） |
| ADR-2 | 元数据库 | SQLite（WAL + 系统 libsqlite3） | PostgreSQL（MVP 期运维过重）；MySQL（需独立服务） |
| ADR-3 | HTTP 框架 | **自研 net 模块**（POSIX socket + 线程池） | Gin/chi（随 ADR-1 一并作废）；cpp-httplib（引入外部依赖，MVP 期不必要） |
| ADR-4 | 数据访问 | sqlite3 C API + Repository 封装 | sqlc（随 ADR-1 作废）；ORM（隐式 SQL、性能不可控） |
| ADR-5 | 桌面 GUI | Qt 6.8 LTS + QML | Electron（包体 100MB+、内存占用高）；Tauri（Rust 心智成本，丧失现有工程积累） |
| ADR-6 | 前端 UI 库 | Element Plus | Ant Design Vue（移动端取向、后台组件密度不如 EP） |
| ADR-7 | 部署形态 | Docker Compose + Caddy | 裸机（环境差异大）；K8s（对单实例产品严重过度设计） |
| ADR-8 | 内容哈希 | SHA-256 | MD5/SHA-1（碰撞风险，去重场景不可接受） |
| ADR-9 | 构建系统 | MVP 沿用 qmake，V1.0 迁 CMake | 立即迁 CMake（MVP 期无收益，徒增风险） |

---

## 7. 许可与合规清单

| 依赖 | 许可证 | 风险 |
|---|---|---|
| SQLite3（系统库） | Public Domain | 无 |
| 服务端自研代码（core / meta / store / net） | 本项目自有 | 无第三方依赖 |
| ffmpeg | LGPL/GPL（视编译选项） | **仅外部进程调用，不链接、不分发二进制** |
| Qt 6.8 LTS | LGPLv3 / 商业许可 | 动态链接并遵守 LGPL 义务；分发前复核 Qt 公司许可条款 |
| Element Plus / Vue / Pinia | MIT | 无 |
| flutter | BSD-3 | 无 |

> 上线发布前必须做一次完整的开源许可证复核，并生成第三方依赖清单随包分发。

---

## 8. 落地节奏

| 阶段 | 需要锁定的选型 |
|---|---|
| 开工前（第 0 周） | ADR-1 ~ ADR-8 全部锁定，CI 骨架与版本号规范就绪 |
| MVP 开发期 | 只用 P0 清单内依赖；新增依赖需走评审 |
| MVP 出口评审 | 复核 P1 清单是否被临时方案污染；启动 CMake 迁移评估 |
| V1.0 开发期 | 引入 Caddy、缩略图、安装包、前端 E2E、指标采集 |
| V1.1+ | 按市场需求选择 P2 项，逐项开关上线 |

---

*本技术栈文档为 v1.0，随 ADR 增删同步更新；任何 P0 级别替换必须重新评审并回写 ADR 表。*
