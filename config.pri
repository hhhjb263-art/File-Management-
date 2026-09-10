# ---------------------------------------------------------------------------
# 全局编译配置：Qt 模块、C++ 标准、输出目录、平台开关
# ---------------------------------------------------------------------------

# Qt 模块
#   core/gui        基础
#   quick/qml       QML 引擎与界面
#   quickcontrols2  控件库（FluentWinUI3 风格）
#   network         REST / 分块上传下载
#   sql             本地同步索引（SQLite）
#   concurrent      后台任务线程池
QT += core gui qml quick quickcontrols2 network sql concurrent

CONFIG += c++17 qtquickcompiler
CONFIG -= debug_and_release

# MSVC 需要显式声明源码为 UTF-8（MinGW 默认按 UTF-8 处理）
*-msvc* {
    QMAKE_CXXFLAGS += /utf-8 /Zc:__cplusplus /permissive-
    DEFINES += _CRT_SECURE_NO_WARNINGS
} else {
    QMAKE_CXXFLAGS += -finput-charset=UTF-8 -fexec-charset=UTF-8
}

DEFINES += UNICODE _UNICODE
DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000

# 统一的头文件搜索路径
INCLUDEPATH += $$PWD/src

# 中间产物目录，保持仓库根目录整洁
OBJECTS_DIR  = $$PWD/build/obj
MOC_DIR      = $$PWD/build/moc
RCC_DIR      = $$PWD/build/rcc
UI_DIR       = $$PWD/build/ui
QMLCACHE_DIR = $$PWD/build/qmlcache

win32 {
    QMAKE_TARGET_PRODUCT   = "云匣 CloudVault"
    QMAKE_TARGET_COMPANY   = "CloudVault"
    QMAKE_TARGET_DESCRIPTION = "个人私有云网盘客户端"
}
