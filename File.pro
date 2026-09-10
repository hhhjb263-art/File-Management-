# ---------------------------------------------------------------------------
#  云匣 CloudVault —— 个人私有云网盘客户端
#  Qt 6 / C++17 + QML
#
#  目录约定（分层，禁止跨层反向依赖）：
#    src/core        基础能力：数据结构、日志、配置、哈希、路径、异步工具
#    src/net         数据源抽象：Backend 接口 / 本地引擎 / 远程 REST 实现
#    src/sync        同步引擎：本地索引、忽略规则、文件监听、双向同步
#    src/transfer    传输队列：分块上传下载、秒传、断点续传
#    src/data        QML 数据模型（QAbstractListModel 派生）
#    src/controllers QML 控制器（界面唯一的 C++ 入口）
#    src/app         应用装配与启动
#    qml             界面：theme / controls / pages / dialogs
#    resources       图标、样式配置
#
#  本文件只做工程装配，具体文件在各模块 .pri 中维护。
# ---------------------------------------------------------------------------

include(config.pri)

TARGET   = CloudVault
TEMPLATE = app
DESTDIR  = $$PWD/bin
VERSION  = 0.5.0

DEFINES += CV_APP_VERSION=\\\"$${VERSION}\\\"

include(src/core/core.pri)
include(src/net/net.pri)
include(src/sync/sync.pri)
include(src/transfer/transfer.pri)
include(src/data/data.pri)
include(src/controllers/controllers.pri)
include(src/app/app.pri)
include(resources/resources.pri)
