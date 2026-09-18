# sync：文件夹自动同步（本地目录 <-> 云端目录）
#
# ⚠️ 阶段说明（2026-09-17 整合）：
#   本阶段（正式客户端 v1）不启用同步功能（SyncController.supported = false），
#   故仅编入已实现的 LocalIndex 与头文件 SyncRules.h。
#   以下三个组件尚未实现，等同步引擎阶段再启用（补 .h/.cpp 后取消注释即可）：
#     FileWatcher.h/.cpp   —— 目录监听
#     SyncEngine.h/.cpp    —— 双向同步调度
#     SyncRules.cpp        —— 忽略规则实现（目前仅有头文件声明）
INCLUDEPATH += $$PWD

HEADERS += \
    $$PWD/LocalIndex.h \
    $$PWD/SyncRules.h

SOURCES += \
    $$PWD/LocalIndex.cpp
