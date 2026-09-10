# core：与业务无关的基础能力，可被任意层引用
INCLUDEPATH += $$PWD

HEADERS += \
    $$PWD/Types.h \
    $$PWD/Async.h \
    $$PWD/Util.h \
    $$PWD/Logging.h \
    $$PWD/AppPaths.h \
    $$PWD/Settings.h \
    $$PWD/Crypto.h

SOURCES += \
    $$PWD/Util.cpp \
    $$PWD/Logging.cpp \
    $$PWD/AppPaths.cpp \
    $$PWD/Settings.cpp \
    $$PWD/Crypto.cpp
