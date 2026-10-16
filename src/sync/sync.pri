# sync：文件夹自动同步（本地目录 <-> 云端目录）
#
# 进度（2026-10-15）：
#   · LocalIndex            已实现（同步索引：上次成功同步时的两端一致状态）
#   · SyncRules.cpp         ✅ 忽略规则（此前只有头文件声明）
#   · SyncPlan.h/.cpp       ✅ 对账决策引擎（方向裁剪 / 冲突另存 / 删除同步）
#   · FileWatcher.h/.cpp    ✅ 递归监听 + 5 秒防抖（FR-3-1）
#   · SyncEngine.h/.cpp     ✅ 调度与执行：启动全量对账 + 每 10 分钟兜底 + 推进索引 + 同步日志
INCLUDEPATH += $$PWD

HEADERS += \
    $$PWD/LocalIndex.h \
    $$PWD/SyncRules.h \
    $$PWD/SyncPlan.h \
    $$PWD/FileWatcher.h \
    $$PWD/SyncEngine.h

SOURCES += \
    $$PWD/LocalIndex.cpp \
    $$PWD/SyncRules.cpp \
    $$PWD/SyncPlan.cpp \
    $$PWD/FileWatcher.cpp \
    $$PWD/SyncEngine.cpp
