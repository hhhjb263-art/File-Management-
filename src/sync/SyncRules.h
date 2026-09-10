/****************************************************************************
 * sync/SyncRules.h —— 同步忽略规则
 *
 * 内置规则（系统垃圾文件、版本控制目录等）+ 同步根目录下 .cvignore 自定义规则。
 * 规则语法：每行一个通配符（* ? []），# 开头为注释，目录名后加 / 表示只匹配目录。
 ****************************************************************************/
#pragma once

#include <QString>
#include <QStringList>

namespace cv {

class SyncRules
{
public:
    static QString     ignoreFileName();
    static QStringList builtinPatterns();
    static QString     defaultIgnoreFileContent();

    // relPath 为相对同步根的路径（'/' 分隔），name 为文件/目录名
    static bool isIgnored(const QString &relPath, const QString &name, bool isDir);

    // 读取同步根目录下的 .cvignore
    static QStringList loadUserPatterns(const QString &syncRoot);

    // 单条规则匹配（支持 * ? 通配符）
    static bool matches(const QString &pattern, const QString &name, bool isDir);
};

} // namespace cv
