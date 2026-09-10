# net：数据源抽象层。上层只依赖 Backend 接口，不关心本地/远程实现
INCLUDEPATH += $$PWD

HEADERS += \
    $$PWD/Backend.h \
    $$PWD/Json.h \
    $$PWD/MockBackend.h \
    $$PWD/HttpBackend.h

SOURCES += \
    $$PWD/Json.cpp \
    $$PWD/MockBackend.cpp \
    $$PWD/HttpBackend.cpp
