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

    // ---- 以下为本次实现时新增的加法式接口（不改动上面既有签名）----

    // 带 relPath 的匹配：模式含 '/' 时按整条相对路径匹配，否则只匹配末段
    static bool matches(const QString &pattern, const QString &relPath, const QString &name,
                        bool isDir);

    // 按给定模式列表判定（逐条、后匹配者胜，支持 '!' 取反）——引擎用这个，
    // 因为内置规则 + .cvignore 必须一起参与判定
    static bool isIgnored(const QStringList &patterns, const QString &relPath, const QString &name,
                          bool isDir);

    // 便捷重载：直接从 relPath 取末段，避免调用方到处 split
    static bool isIgnoredPath(const QStringList &patterns, const QString &relPath, bool isDir);

    // 内置 + 该同步根的 .cvignore（顺序：内置在前，用户规则在后 ⇒ 用户可用 '!' 覆盖内置）
    static QStringList effectivePatterns(const QString &syncRoot);
};

} // namespace cv
