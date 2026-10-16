/****************************************************************************
 * sync/SyncRules.cpp —— 同步忽略规则实现（PRD FR-3-4）
 *
 * 这是本文件第一次被实现：此前只有 SyncRules.h 的声明，sync.pri 里也明写着
 * "SyncRules.cpp —— 忽略规则实现（目前仅有头文件声明）"。
 *
 * 规则语法（gitignore 子集，刻意实现得可预测）：
 *   · 每行一个模式；空行与 `#` 开头为注释
 *   · `*` `?` `[...]` 通配符 —— **不跨 '/'**（这是与 shell glob 的关键一致点）
 *   · 模式含 '/' → 与整条相对路径匹配；不含 '/' → 只与**路径末段**（文件/目录名）匹配
 *     于是 `*.tmp` 能命中任意层级，而 `src/` 这种带斜杠的模式只命中该目录那一层
 *     （注意：源码注释里不要出现斜杠加星号，会被 GCC 当成未闭合注释块而告警）
 *   · 结尾 '/' → **只匹配目录**
 *   · 开头 '!' → 取反（取消忽略）；按**从上到下、后匹配者胜**（last-match-wins）
 *   · '\' 转义下一个字符
 *
 * ⚠️ 目录命中要连带其子孙：调用方遍历时若某目录被判为忽略，必须**整棵子树跳过**，
 *    否则会出现"目录被忽略但里面的文件被同步上来"这种半截状态（引擎侧保证）。
 ****************************************************************************/
#include "SyncRules.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

namespace cv {
namespace {

// 单字符类匹配：pat[open] == '['，返回是否命中，matchedEnd 为 ']' 之后的位置（未闭合则返回 -1）
bool matchClass(const QString &pat, int open, QChar ch, int *matchedEnd)
{
    int i = open + 1;
    bool negate = false;
    if (i < pat.size() && (pat[i] == QLatin1Char('!') || pat[i] == QLatin1Char('^'))) {
        negate = true;
        ++i;
    }

    bool hit = false;
    bool first = true;   // ']' 作为首字符时是字面量，不算闭合
    while (i < pat.size()) {
        const QChar c = pat[i];
        if (c == QLatin1Char(']') && !first)
            break;

        // 区间 a-z
        if (i + 2 < pat.size() && pat[i + 1] == QLatin1Char('-') && pat[i + 2] != QLatin1Char(']')) {
            const QChar lo = c;
            const QChar hi = pat[i + 2];
            if (ch >= lo && ch <= hi)
                hit = true;
            i += 3;
            first = false;
            continue;
        }

        if (c == ch)
            hit = true;
        ++i;
        first = false;
    }
    if (i >= pat.size() || pat[i] != QLatin1Char(']'))
        return false;   // 未闭合：整条模式按不匹配处理（宁可漏忽略也不误忽略）

    if (matchedEnd)
        *matchedEnd = i + 1;
    return negate ? !hit : hit;
}

// 段内 glob：'*' '?' '[...]' 均**不跨 '/'**，带 '*' 回溯
bool globMatch(const QString &pat, const QString &text)
{
    int p = 0, t = 0;
    int star = -1, mark = 0;

    while (t < text.size()) {
        bool advanced = false;
        if (p < pat.size()) {
            const QChar pc = pat[p];
            const QChar tc = text[t];

            if (pc == QLatin1Char('\\') && p + 1 < pat.size()) {
                if (pat[p + 1] == tc) { p += 2; ++t; advanced = true; }
            } else if (pc == QLatin1Char('?')) {
                if (tc != QLatin1Char('/')) { ++p; ++t; advanced = true; }
            } else if (pc == QLatin1Char('[')) {
                int end = -1;
                if (tc != QLatin1Char('/') && matchClass(pat, p, tc, &end)) {
                    p = end; ++t; advanced = true;
                }
            } else if (pc == QLatin1Char('*')) {
                star = p++;
                mark = t;
                advanced = true;   // '*' 可匹配空串，先按零宽前进
                continue;
            } else if (pc == tc) {
                ++p; ++t; advanced = true;
            }
        }

        if (advanced)
            continue;

        // 回溯：上一个 '*' 多吃一个字符（但不吃 '/'）
        if (star >= 0 && text[mark] != QLatin1Char('/')) {
            p = star + 1;
            t = ++mark;
            continue;
        }
        return false;
    }

    while (p < pat.size() && pat[p] == QLatin1Char('*'))
        ++p;
    return p == pat.size();
}

struct Parsed
{
    QString body;             // 去掉 !、结尾 '/' 与首尾空白后的模式
    bool    negate = false;
    bool    dirOnly = false;
    bool    hasSlash = false;
    bool    valid = false;
};

Parsed parsePattern(const QString &rawLine)
{
    Parsed r;
    QString line = rawLine.trimmed();
    if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
        return r;

    if (line.startsWith(QLatin1Char('!'))) {
        r.negate = true;
        line = line.mid(1).trimmed();
        if (line.isEmpty())
            return r;
    }
    if (line.endsWith(QLatin1Char('/'))) {
        r.dirOnly = true;
        line.chop(1);
    }
    if (line.isEmpty())
        return r;

    r.body = line;
    r.hasSlash = line.contains(QLatin1Char('/'));
    r.valid = true;
    return r;
}

// 单个模式对 (relPath, name, isDir) 的判定
bool patternHits(const QString &rawLine, const QString &relPath, const QString &name, bool isDir)
{
    const Parsed p = parsePattern(rawLine);
    if (!p.valid)
        return false;
    if (p.dirOnly && !isDir)
        return false;

    const QString target = p.hasSlash ? relPath : name;
    if (globMatch(p.body, target))
        return true;

    // gitignore 习惯：`build` 这种不含 '/' 的模式应当同时命中"名为 build 的目录"
    // 及其下所有内容 —— 后者由调用方在遍历时整棵跳过（见文件头说明），此处不重复展开。
    return false;
}

} // namespace

QString SyncRules::ignoreFileName()
{
    return QStringLiteral(".cvignore");
}

QStringList SyncRules::builtinPatterns()
{
    // PRD FR-3-4 点名的：Thumbs.db、.DS_Store、*.tmp、~$*、系统休眠文件。
    // 其余为同族常见垃圾/元数据，一并内置（避免用户每台机器都要自己写一遍）。
    return QStringList{
        QStringLiteral("Thumbs.db"),
        QStringLiteral("desktop.ini"),
        QStringLiteral(".DS_Store"),
        QStringLiteral("._*"),                 // macOS 资源分支
        QStringLiteral(".Spotlight-V100/"),
        QStringLiteral(".Trashes/"),
        QStringLiteral("*.tmp"),
        QStringLiteral("*.temp"),
        QStringLiteral("~$*"),                 // Office 临时/锁文件
        QStringLiteral("*.swp"),
        QStringLiteral("*.swo"),
        QStringLiteral("*~"),                  // 编辑器备份
        QStringLiteral("*.part"),              // 下载中间态
        QStringLiteral("*.crdownload"),
        QStringLiteral("hiberfil.sys"),        // Windows 休眠文件
        QStringLiteral("pagefile.sys"),
        QStringLiteral("swapfile.sys"),
        QStringLiteral(".git/"),               // 版本控制目录
        QStringLiteral(".svn/"),
        QStringLiteral(".hg/"),
        ignoreFileName(),                      // .cvignore 本身是"本机策略"，不同步
                                               // （注意：这里不能写 QStringLiteral(ignoreFileName())
                                               //   —— 该宏只接受字符串字面量，包函数调用会编译失败）
    };
}

QString SyncRules::defaultIgnoreFileContent()
{
    return QStringLiteral(
        "# 云匣同步忽略规则（语法：gitignore 子集）\n"
        "#\n"
        "#   *.tmp          任意层级的 .tmp\n"
        "#   build/         只匹配目录（连同其下所有内容一起跳过）\n"
        "#   src/*.o        含 '/' 则与整条相对路径匹配（此处只匹配 src 下一层）\n"
        "#   !keep.tmp      取反：取消忽略（后匹配者胜，所以取反要写在后面）\n"
        "#   # 开头的行是注释；空行忽略\n"
        "#\n"
        "# 内置已忽略：Thumbs.db / desktop.ini / .DS_Store / *.tmp / ~$* /\n"
        "#             *.swp / *~ / hiberfil.sys / .git/ / .svn/ 等（无需在此重复）\n"
        "\n"
        "# 在这里追加你自己的规则，例如：\n"
        "# 我的素材/大文件/\n"
        "# *.iso\n");
}

bool SyncRules::matches(const QString &pattern, const QString &name, bool isDir)
{
    return patternHits(pattern, name, name, isDir);
}

bool SyncRules::matches(const QString &pattern, const QString &relPath, const QString &name,
                        bool isDir)
{
    return patternHits(pattern, relPath, name, isDir);
}

bool SyncRules::isIgnored(const QString &relPath, const QString &name, bool isDir)
{
    return isIgnored(builtinPatterns(), relPath, name, isDir);
}

bool SyncRules::isIgnored(const QStringList &patterns, const QString &relPath, const QString &name,
                          bool isDir)
{
    // last-match-wins：逐条判定，命中就翻转状态（含 '!' 取反）
    bool ignored = false;
    for (const QString &line : patterns) {
        if (patternHits(line, relPath, name, isDir)) {
            const Parsed p = parsePattern(line);
            ignored = !p.negate;
        }
    }
    return ignored;
}

QStringList SyncRules::loadUserPatterns(const QString &syncRoot)
{
    QStringList out;
    if (syncRoot.isEmpty())
        return out;

    QFile f(QDir(syncRoot).filePath(ignoreFileName()));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return out;   // 没有 .cvignore 是完全正常的，不是错误

    QTextStream in(&f);
    in.setEncoding(QStringConverter::Utf8);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        out << line;
    }
    return out;
}

QStringList SyncRules::effectivePatterns(const QString &syncRoot)
{
    QStringList all = builtinPatterns();
    all << loadUserPatterns(syncRoot);
    return all;
}

bool SyncRules::isIgnoredPath(const QStringList &patterns, const QString &relPath, bool isDir)
{
    const int slash = relPath.lastIndexOf(QLatin1Char('/'));
    const QString name = slash < 0 ? relPath : relPath.mid(slash + 1);
    return isIgnored(patterns, relPath, name, isDir);
}

} // namespace cv
