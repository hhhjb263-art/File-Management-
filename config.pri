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

# 中间产物目录：必须按「构建目录」隔离（$$OUT_PWD），否则多个构建目录（build_official /
# build_qa / 任意新目录）会共用同一批 .o 与 moc，导致"看起来重新构建、其实只是重链"——
# 这会掩盖真实编译错误，让验证假绿。DESTDIR 仍指向源码树的 bin/（保持产物位置唯一可预期）。
OBJECTS_DIR  = $$OUT_PWD/build/obj
MOC_DIR      = $$OUT_PWD/build/moc
RCC_DIR      = $$OUT_PWD/build/rcc
UI_DIR       = $$OUT_PWD/build/ui
QMLCACHE_DIR = $$OUT_PWD/build/qmlcache

win32 {
    QMAKE_TARGET_PRODUCT   = "云匣 CloudVault"
    QMAKE_TARGET_COMPANY   = "CloudVault"
    QMAKE_TARGET_DESCRIPTION = "个人私有云网盘客户端"
}
