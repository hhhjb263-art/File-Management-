/****************************************************************************
 * core/Util.h —— 纯工具函数（格式化 / 类型判定 / 路径处理）
 * 以 QML 上下文属性 "Util" 暴露给界面使用。
 ****************************************************************************/
#pragma once

#include "Types.h"

#include <QObject>
#include <QString>

namespace cv {

class Util : public QObject
{
    Q_OBJECT
public:
    explicit Util(QObject *parent = nullptr) : QObject(parent) {}

    // ---- 格式化 ----
    Q_INVOKABLE static QString humanSize(qint64 bytes);
    Q_INVOKABLE static QString humanSpeed(double bytesPerSec);
    Q_INVOKABLE static QString relativeTime(const QDateTime &dt); // "刚刚 / 3 分钟前 / 昨天"
    Q_INVOKABLE static QString formatTime(const QDateTime &dt);   // "2026-09-04 09:00"
    Q_INVOKABLE static QString formatDate(const QDateTime &dt);   // "2026-09-04"

    // ---- 类型 ----
    Q_INVOKABLE static QString typeName(int type);                // FileType -> 中文名
    Q_INVOKABLE static QString typeIcon(int type);                // FileType -> 图标路径
    static FileType fileTypeOf(const QString &name, bool isDir);
    Q_INVOKABLE static QString fileKindOf(const QString &name);   // 供 QML：由文件名得到类型名

    // ---- 路径 ----
    Q_INVOKABLE static QString urlToLocalPath(const QString &url); // QML url -> 本地路径
    static QString joinPath(const QString &dir, const QString &name);
    static QString uniquePath(const QString &path);                // 存在则追加 (1)/(2)
    static QString conflictPath(const QString &path, const QString &device);
    static QString sanitizeName(const QString &name);

    // ---- 其它 ----
    Q_INVOKABLE static QString deviceName();
    Q_INVOKABLE static QString initialsOf(const QString &name);
};

} // namespace cv
