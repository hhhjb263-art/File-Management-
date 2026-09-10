/****************************************************************************
 * net/Json.h —— 数据结构 <-> JSON 的统一映射
 * 本地引擎（落盘）与远程服务（REST）共用同一套序列化规则。
 ****************************************************************************/
#pragma once

#include "core/Types.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

namespace cv {
namespace Json {

QString    dtToString(const QDateTime &dt);
QDateTime  dtFromString(const QString &s);

QJsonObject toJson(const FileItem &f);
FileItem    fileFromJson(const QJsonObject &o);

QJsonObject toJson(const FileVersion &v);
FileVersion versionFromJson(const QJsonObject &o);

QJsonObject toJson(const ShareLink &s);
ShareLink   shareFromJson(const QJsonObject &o);

QJsonObject toJson(const Tag &t);
Tag         tagFromJson(const QJsonObject &o);

QJsonObject toJson(const UserInfo &u);
UserInfo    userFromJson(const QJsonObject &o);

QJsonObject toJson(const UsageStats &s);
UsageStats  usageFromJson(const QJsonObject &o);

// 列表批量转换
template <typename T, typename ToJson, typename FromJson>
QJsonArray toArray(const QVector<T> &list, ToJson toJsonFn)
{
    QJsonArray arr;
    for (const T &item : list)
        arr.append(toJsonFn(item));
    return arr;
}

template <typename T>
QVector<T> fromArray(const QJsonValue &value, T (*fromJsonFn)(const QJsonObject &))
{
    QVector<T> out;
    const QJsonArray arr = value.isArray() ? value.toArray() : QJsonArray();
    for (const QJsonValue &v : arr)
        out.append(fromJsonFn(v.toObject()));
    return out;
}

// 排序：目录在前，名称次之
bool fileLess(const FileItem &a, const FileItem &b);

} // namespace Json
} // namespace cv
