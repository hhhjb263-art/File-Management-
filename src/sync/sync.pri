# sync：文件夹自动同步（本地目录 <-> 云端目录）
INCLUDEPATH += $$PWD

HEADERS += \
    $$PWD/LocalIndex.h \
    $$PWD/SyncRules.h \
    $$PWD/FileWatcher.h \
    $$PWD/SyncEngine.h

SOURCES += \
    $$PWD/LocalIndex.cpp \
    $$PWD/SyncRules.cpp \
    $$PWD/FileWatcher.cpp \
    $$PWD/SyncEngine.cpp
