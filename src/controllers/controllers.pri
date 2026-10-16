# controllers：QML 与 C++ 之间的唯一桥梁，界面只调用控制器，不直接碰后端
INCLUDEPATH += $$PWD

HEADERS += \
    $$PWD/AppController.h \
    $$PWD/AuthController.h \
    $$PWD/FileController.h \
    $$PWD/SyncController.h \
    $$PWD/TransferController.h \
    $$PWD/ShareController.h \
    $$PWD/StatsController.h \
    $$PWD/TrashController.h \
    $$PWD/TagController.h \
    $$PWD/VersionController.h

SOURCES += \
    $$PWD/AppController.cpp \
    $$PWD/AuthController.cpp \
    $$PWD/FileController.cpp \
    $$PWD/SyncController.cpp \
    $$PWD/TransferController.cpp \
    $$PWD/ShareController.cpp \
    $$PWD/StatsController.cpp \
    $$PWD/TrashController.cpp \
    $$PWD/TagController.cpp \
    $$PWD/VersionController.cpp
