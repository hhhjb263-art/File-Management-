// 同步对账引擎（SyncPlan）与忽略规则（SyncRules）的验证 —— 只依赖 QtCore，可独立编译。
//
// 为什么单独造一套 harness：这是 F3 里唯一会**静默毁掉用户数据**的模块
//   · 判错"哪一侧改了" → 覆盖掉用户的新内容
//   · 漏判冲突        → 静默丢一份
//   · 忽略规则写宽    → 该同步的没同步
// 所以每条 PRD 需求（FR-3-2/3-4/3-5/3-6）都对应可复现的用例。
//
// 编译（Windows mingw）：
//   export PATH="/d/Qt/6.9.3/mingw_64/bin:/d/Qt/Tools/mingw1310_64/bin:$PATH"
//   g++ -std=gnu++1z -static -static-libgcc -static-libstdc++ -fexceptions -mthreads \
//       -DUNICODE -D_UNICODE -DWIN32 -DQT_NO_DEBUG \
//       -I src -I src/core -I src/sync \
//       -I "D:/Qt/6.9.3/mingw_64/include" -I "D:/Qt/6.9.3/mingw_64/include/QtCore" \
//       .localcheck/sync_plan_harness.cpp src/sync/SyncPlan.cpp src/sync/SyncRules.cpp \
//       "D:/Qt/6.9.3/mingw_64/lib/libQt6Core.a" \
//       -lmingw32 -lws2_32 -lversion -lwinmm -lole32 -luuid -lbcrypt -luserenv -lshell32 \
//       -o sync_plan_harness.exe
//
// ⚠️ 两点必须注意（都踩过）：
//   1) `-static -static-libgcc -static-libstdc++` **不可省**，否则运行时报缺 libstdc++-6.dll。
//   2) Qt6Core 在 mingw 上是**动态**链接的 —— `libQt6Core.a` 其实是导入库（不是静态库），
//      所以**运行时要把 Qt 的 bin 放进 PATH**，否则报 "error while loading shared libraries: Qt6Core.dll"。
//      上面把 PATH 设好再跑就行。
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <cstdio>

#include "sync/SyncPlan.h"
#include "sync/SyncRules.h"

using cv::SyncAction;
using cv::SyncMode;
using cv::SyncPlan;
using cv::SyncPlanInput;
using cv::SyncPlanResult;
using cv::SyncRules;
using cv::SyncSnapshot;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg)                          \
    do {                                          \
        if (cond) {                               \
            ++g_pass;                             \
        } else {                                  \
            ++g_fail;                             \
            std::printf("  FAIL: %s\n", msg);     \
        }                                         \
    } while (0)

// 固定时间基准，保证命名可复现
static const QDateTime kNow = QDateTime::fromString(QStringLiteral("2026-10-15T11:22:33Z"), Qt::ISODateWithMs);
static const QDateTime kOld = QDateTime::fromString(QStringLiteral("2026-10-01T00:00:00Z"), Qt::ISODateWithMs);
static const QDateTime kNew = QDateTime::fromString(QStringLiteral("2026-10-14T00:00:00Z"), Qt::ISODateWithMs);

static void addFile(SyncSnapshot &s, const QString &p, qint64 size, const QString &hash,
                    const QDateTime &mtime, const QString &rev = QString())
{
    SyncSnapshot::Item it;
    it.size  = size;
    it.hash  = hash;
    it.mtime = mtime;
    it.rev   = rev;
    s.items.insert(p, it);
}

static void addDir(SyncSnapshot &s, const QString &p)
{
    SyncSnapshot::Item it;
    it.isDir = true;
    s.items.insert(p, it);
}

// 找某个 relPath 的动作；找不到返回 nullptr
static const SyncAction *findAct(const SyncPlanResult &r, const QString &p)
{
    for (const SyncAction &a : r.actions) {
        if (a.relPath == p)
            return &a;
    }
    return nullptr;
}

static bool kindIs(const SyncPlanResult &r, const QString &p, SyncAction::Kind k)
{
    const SyncAction *a = findAct(r, p);
    return a && a->kind == k;
}

static const char *kindName(SyncAction::Kind k)
{
    switch (k) {
    case SyncAction::UploadNew:           return "UploadNew";
    case SyncAction::UploadUpdate:        return "UploadUpdate";
    case SyncAction::DownloadNew:         return "DownloadNew";
    case SyncAction::DownloadUpdate:      return "DownloadUpdate";
    case SyncAction::DeleteRemote:        return "DeleteRemote";
    case SyncAction::DeleteLocal:         return "DeleteLocal";
    case SyncAction::ConflictKeepLocal:   return "ConflictKeepLocal";
    case SyncAction::ConflictKeepRemote:  return "ConflictKeepRemote";
    case SyncAction::MakeLocalDir:        return "MakeLocalDir";
    case SyncAction::MakeRemoteDir:       return "MakeRemoteDir";
    case SyncAction::Skip:                return "Skip";
    }
    return "?";
}

// 打印某路径的判定（失败时便于定位）
static void dump(const SyncPlanResult &r, const QString &p)
{
    const SyncAction *a = findAct(r, p);
    std::printf("    [%s] -> %s (%s)\n", qPrintable(p),
                a ? kindName(a->kind) : "<无动作>", a ? qPrintable(a->reason) : "");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // ==================================================================
    std::printf("== A. 无基准（新增）：方向裁剪 ==\n");
    {
        SyncPlanInput in;
        in.now        = kNow;
        in.deviceName = QStringLiteral("PC-01");
        in.local.items.clear();
        in.remote.items.clear();
        in.lastSynced.items.clear();
        addFile(in.local, QStringLiteral("local-new.txt"), 10, QStringLiteral("hL"), kNew);
        addFile(in.remote, QStringLiteral("remote-new.txt"), 20, QStringLiteral("hR"), kNew);
        addDir(in.local, QStringLiteral("newdir"));

        in.mode = SyncMode::TwoWay;
        SyncPlanResult r = SyncPlan::build(in);
        CHECK(kindIs(r, QStringLiteral("local-new.txt"), SyncAction::UploadNew), "A1 双向：本地新增→上传");
        CHECK(kindIs(r, QStringLiteral("remote-new.txt"), SyncAction::DownloadNew), "A2 双向：云端新增→下载");
        CHECK(kindIs(r, QStringLiteral("newdir"), SyncAction::MakeRemoteDir), "A3 目录新增→建云端目录");

        in.mode = SyncMode::UploadOnly;
        r = SyncPlan::build(in);
        CHECK(kindIs(r, QStringLiteral("local-new.txt"), SyncAction::UploadNew), "A4 仅上传：本地新增仍上传");
        CHECK(kindIs(r, QStringLiteral("remote-new.txt"), SyncAction::Skip), "A5 仅上传：忽略云端新增");

        in.mode = SyncMode::DownloadOnly;
        r = SyncPlan::build(in);
        CHECK(kindIs(r, QStringLiteral("local-new.txt"), SyncAction::Skip), "A6 仅下载：忽略本地新增");
        CHECK(kindIs(r, QStringLiteral("remote-new.txt"), SyncAction::DownloadNew), "A7 仅下载：云端新增下载");
    }
    {
        // 两端各自新增且内容一致
        SyncPlanInput in;
        in.now = kNow;
        in.deviceName = QStringLiteral("PC-01");
        addFile(in.local, QStringLiteral("both.txt"), 7, QStringLiteral("same"), kNew);
        addFile(in.remote, QStringLiteral("both.txt"), 7, QStringLiteral("same"), kOld);
        SyncPlanResult r = SyncPlan::build(in);
        CHECK(kindIs(r, QStringLiteral("both.txt"), SyncAction::Skip), "A8 两端新增且内容一致→跳过");

        // 两端各自新增且内容不同 → 冲突（本地较新）
        in.local.items.clear();
        in.remote.items.clear();
        addFile(in.local, QStringLiteral("c.txt"), 7, QStringLiteral("hA"), kNew);
        addFile(in.remote, QStringLiteral("c.txt"), 7, QStringLiteral("hB"), kOld);
        r = SyncPlan::build(in);
        CHECK(kindIs(r, QStringLiteral("c.txt"), SyncAction::ConflictKeepLocal),
              "A9 两端新增内容不同且本地较新→本地为主");
    }

    // ==================================================================
    std::printf("== B. 单侧修改 ==\n");
    {
        SyncPlanInput in;
        in.now = kNow;
        in.deviceName = QStringLiteral("PC-01");
        addFile(in.lastSynced, QStringLiteral("f.txt"), 10, QStringLiteral("base"), kOld, QStringLiteral("r1"));
        addFile(in.local, QStringLiteral("f.txt"), 11, QStringLiteral("changed"), kNew, QStringLiteral("r1"));
        addFile(in.remote, QStringLiteral("f.txt"), 10, QStringLiteral("base"), kOld, QStringLiteral("r1"));

        SyncPlanResult r = SyncPlan::build(in);
        CHECK(kindIs(r, QStringLiteral("f.txt"), SyncAction::UploadUpdate), "B1 本地改了→上传");

        // 反向：云端改了
        in.local.items.clear();
        addFile(in.local, QStringLiteral("f.txt"), 10, QStringLiteral("base"), kOld, QStringLiteral("r1"));
        in.remote.items.clear();
        addFile(in.remote, QStringLiteral("f.txt"), 12, QStringLiteral("srv"), kNew, QStringLiteral("r2"));
        r = SyncPlan::build(in);
        CHECK(kindIs(r, QStringLiteral("f.txt"), SyncAction::DownloadUpdate), "B2 云端改了→下载");

        // 仅上传模式忽略云端改动
        in.mode = SyncMode::UploadOnly;
        r = SyncPlan::build(in);
        CHECK(kindIs(r, QStringLiteral("f.txt"), SyncAction::Skip), "B3 仅上传：忽略云端改动");

        // 仅上传模式：本地改了就上传
        in.mode = SyncMode::UploadOnly;
        in.local.items.clear();
        addFile(in.local, QStringLiteral("f.txt"), 99, QStringLiteral("locnew"), kNew, QStringLiteral("r1"));
        in.remote.items.clear();
        addFile(in.remote, QStringLiteral("f.txt"), 10, QStringLiteral("base"), kOld, QStringLiteral("r1"));
        r = SyncPlan::build(in);
        CHECK(kindIs(r, QStringLiteral("f.txt"), SyncAction::UploadUpdate), "B4 仅上传：本地改了照常上传");

        // 两端都没变
        in.mode = SyncMode::TwoWay;
        in.local.items.clear();
        addFile(in.local, QStringLiteral("f.txt"), 10, QStringLiteral("base"), kOld, QStringLiteral("r1"));
        in.remote.items.clear();
        addFile(in.remote, QStringLiteral("f.txt"), 10, QStringLiteral("base"), kOld, QStringLiteral("r1"));
        r = SyncPlan::build(in);
        CHECK(kindIs(r, QStringLiteral("f.txt"), SyncAction::Skip), "B5 两端均未变→跳过");
    }

    // ==================================================================
    std::printf("== C. 删除同步（FR-3-6）==\n");
    {
        SyncPlanInput in;
        in.now = kNow;
        in.deviceName = QStringLiteral("PC-01");
        addFile(in.lastSynced, QStringLiteral("d.txt"), 5, QStringLiteral("base"), kOld);
        addFile(in.remote, QStringLiteral("d.txt"), 5, QStringLiteral("base"), kOld);
        // local 里没有 → 本地已删
        SyncPlanResult r = SyncPlan::build(in);
        CHECK(kindIs(r, QStringLiteral("d.txt"), SyncAction::DeleteRemote), "C1 本地删、云端未变→删云端");

        // 云端删、本地未变
        in = SyncPlanInput();
        in.now = kNow;
        in.deviceName = QStringLiteral("PC-01");
        addFile(in.lastSynced, QStringLiteral("d.txt"), 5, QStringLiteral("base"), kOld);
        addFile(in.local, QStringLiteral("d.txt"), 5, QStringLiteral("base"), kOld);
        r = SyncPlan::build(in);
        CHECK(kindIs(r, QStringLiteral("d.txt"), SyncAction::DeleteLocal), "C2 云端删、本地未变→删本地");

        // 关掉删除同步 → 只解除同步，两端都保留
        in.deleteSyncEnabled = false;
        r = SyncPlan::build(in);
        CHECK(kindIs(r, QStringLiteral("d.txt"), SyncAction::Skip), "C3 关闭删除同步→仅解除同步（不删本地）");
        CHECK(r.counts.delLocal == 0 && r.counts.delRemote == 0, "C4 关闭删除同步后无任何删除动作");

        // 本地删了但云端改了 → 必须以云端为准恢复，绝不能删云端
        in = SyncPlanInput();
        in.now = kNow;
        in.deviceName = QStringLiteral("PC-01");
        addFile(in.lastSynced, QStringLiteral("d.txt"), 5, QStringLiteral("base"), kOld, QStringLiteral("r1"));
        addFile(in.remote, QStringLiteral("d.txt"), 9, QStringLiteral("srvnew"), kNew, QStringLiteral("r2"));
        r = SyncPlan::build(in);
        CHECK(kindIs(r, QStringLiteral("d.txt"), SyncAction::DownloadUpdate),
              "C5 本地已删但云端有更新→以云端为准恢复（不丢云端改动）");
        CHECK(r.counts.delRemote == 0, "C6 该场景下不得产生 DeleteRemote");

        // 云端删了但本地改了 → 以本地为准重新上传
        in = SyncPlanInput();
        in.now = kNow;
        in.deviceName = QStringLiteral("PC-01");
        addFile(in.lastSynced, QStringLiteral("d.txt"), 5, QStringLiteral("base"), kOld);
        addFile(in.local, QStringLiteral("d.txt"), 9, QStringLiteral("locnew"), kNew);
        r = SyncPlan::build(in);
        CHECK(kindIs(r, QStringLiteral("d.txt"), SyncAction::UploadUpdate),
              "C7 云端已删但本地有更新→以本地为准重新上传（不丢本地改动）");

        // 两端都删
        in = SyncPlanInput();
        in.now = kNow;
        in.deviceName = QStringLiteral("PC-01");
        addFile(in.lastSynced, QStringLiteral("d.txt"), 5, QStringLiteral("base"), kOld);
        r = SyncPlan::build(in);
        CHECK(kindIs(r, QStringLiteral("d.txt"), SyncAction::Skip), "C8 两端均已删除→跳过");
    }

    // ==================================================================
    std::printf("== D. 冲突处理（FR-3-5：绝不静默覆盖）==\n");
    {
        SyncPlanInput in;
        in.now = kNow;
        in.deviceName = QStringLiteral("PC-01");
        addFile(in.lastSynced, QStringLiteral("docs/报告.docx"), 10, QStringLiteral("base"), kOld);
        addFile(in.local, QStringLiteral("docs/报告.docx"), 11, QStringLiteral("local"), kNew);
        addFile(in.remote, QStringLiteral("docs/报告.docx"), 12, QStringLiteral("srv"), kOld);

        SyncPlanResult r = SyncPlan::build(in);
        const SyncAction *a = findAct(r, QStringLiteral("docs/报告.docx"));
        CHECK(a && a->kind == SyncAction::ConflictKeepLocal,
              "D1 两端都改且本地较新→本地为主");
        CHECK(a && a->conflictPath == QStringLiteral("docs/报告(冲突-PC-01-20261015-192233).docx"),
              "D2 冲突副本与主文件同目录，且按 文件名(冲突-设备-时间).ext 命名");
        CHECK(r.counts.conflicts == 1, "D3 冲突计数为 1");

        // 云端较新
        in.local.items.clear();
        in.remote.items.clear();
        addFile(in.local, QStringLiteral("docs/报告.docx"), 11, QStringLiteral("local"), kOld);
        addFile(in.remote, QStringLiteral("docs/报告.docx"), 12, QStringLiteral("srv"), kNew);
        r = SyncPlan::build(in);
        a = findAct(r, QStringLiteral("docs/报告.docx"));
        CHECK(a && a->kind == SyncAction::ConflictKeepRemote,
              "D4 两端都改且云端较新→云端为主（本地留副本）");

        // 两端都改但结果一致
        in.local.items.clear();
        in.remote.items.clear();
        addFile(in.local, QStringLiteral("docs/报告.docx"), 11, QStringLiteral("same"), kNew);
        addFile(in.remote, QStringLiteral("docs/报告.docx"), 11, QStringLiteral("same"), kOld);
        r = SyncPlan::build(in);
        CHECK(kindIs(r, QStringLiteral("docs/报告.docx"), SyncAction::Skip),
              "D5 两端各自修改但结果一致→跳过（无需传内容）");

        // 类型冲突：一端目录一端文件 → 不自动合并
        in = SyncPlanInput();
        in.now = kNow;
        in.deviceName = QStringLiteral("PC-01");
        addFile(in.lastSynced, QStringLiteral("x"), 1, QStringLiteral("base"), kOld);
        addDir(in.local, QStringLiteral("x"));
        addFile(in.remote, QStringLiteral("x"), 2, QStringLiteral("srv"), kNew);
        r = SyncPlan::build(in);
        CHECK(kindIs(r, QStringLiteral("x"), SyncAction::Skip),
              "D6 目录/文件类型冲突→显式跳过（自动合并可能毁掉整棵子树）");
    }

    // ==================================================================
    std::printf("== E. 冲突副本命名 ==\n");
    {
        CHECK(SyncPlan::conflictName(QStringLiteral("报告.docx"), QStringLiteral("PC-01"), kNow)
                  == QStringLiteral("报告(冲突-PC-01-20261015-192233).docx"),
              "E1 常规：stem(冲突-设备-时间).ext");
        CHECK(SyncPlan::conflictName(QStringLiteral("归档.tar.gz"), QStringLiteral("PC-01"), kNow)
                  == QStringLiteral("归档.tar(冲突-PC-01-20261015-192233).gz"),
              "E2 多重扩展名只切最后一个点");
        CHECK(SyncPlan::conflictName(QStringLiteral("README"), QStringLiteral("PC-01"), kNow)
                  == QStringLiteral("README(冲突-PC-01-20261015-192233)"),
              "E3 无扩展名不硬加多余的点");
        CHECK(SyncPlan::conflictName(QStringLiteral("a.txt"), QStringLiteral("PC:01*2"), kNow)
                  == QStringLiteral("a(冲突-PC_01_2-20261015-192233).txt"),
              "E4 设备名里的非法路径字符被替换（否则副本可能创建不出来）");
        CHECK(SyncPlan::conflictName(QStringLiteral("a.txt"), QString(), kNow)
                  == QStringLiteral("a(冲突-未知设备-20261015-192233).txt"),
              "E5 设备名缺失时回退，不留空段");
        CHECK(SyncPlan::conflictName(QStringLiteral(".gitignore"), QStringLiteral("PC-01"), kNow)
                  == QStringLiteral(".gitignore(冲突-PC-01-20261015-192233)"),
              "E6 点开头的隐藏文件不被误当扩展名");
    }

    // ==================================================================
    std::printf("== F. 顺序与确定性 ==\n");
    {
        SyncPlanInput in;
        in.now = kNow;
        in.deviceName = QStringLiteral("PC-01");
        addFile(in.local, QStringLiteral("b.txt"), 1, QStringLiteral("h"), kNew);
        addDir(in.local, QStringLiteral("zdir"));
        addFile(in.local, QStringLiteral("a.txt"), 1, QStringLiteral("h"), kNew);

        SyncPlanResult r = SyncPlan::build(in);
        CHECK(r.actions.size() == 3, "F1 三个动作都在");
        CHECK(r.actions[0].kind == SyncAction::MakeRemoteDir,
              "F2 目录动作必须排在文件动作之前（否则先写文件后建目录必然失败）");
        CHECK(r.actions[1].relPath == QStringLiteral("a.txt")
                  && r.actions[2].relPath == QStringLiteral("b.txt"),
              "F3 同层按路径排序 → 输出可复现");

        SyncPlanResult r2 = SyncPlan::build(in);
        CHECK(r2.actions.size() == r.actions.size() && r2.actions[1].relPath == r.actions[1].relPath,
              "F4 同一输入两次结果一致");
        CHECK(r.counts.up == 2 && r.counts.dirs == 1, "F5 计数正确");
    }

    // ==================================================================
    std::printf("== G. 忽略规则（FR-3-4）==\n");
    {
        const QStringList builtin = SyncRules::builtinPatterns();

        CHECK(SyncRules::isIgnoredPath(builtin, QStringLiteral("Thumbs.db"), false), "G1 Thumbs.db 命中");
        CHECK(SyncRules::isIgnoredPath(builtin, QStringLiteral("a/b/.DS_Store"), false), "G2 任意层级 .DS_Store 命中");
        CHECK(SyncRules::isIgnoredPath(builtin, QStringLiteral("x/y/z.tmp"), false), "G3 任意层级 *.tmp 命中");
        CHECK(SyncRules::isIgnoredPath(builtin, QStringLiteral("~$v2.docx"), false), "G4 Office 临时文件命中");
        CHECK(SyncRules::isIgnoredPath(builtin, QStringLiteral("hiberfil.sys"), false), "G5 休眠文件命中");
        CHECK(SyncRules::isIgnoredPath(builtin, QStringLiteral(".git"), true), "G6 .git/ 命中目录");
        CHECK(!SyncRules::isIgnoredPath(builtin, QStringLiteral("报告.docx"), false), "G7 普通文件不误伤");
        CHECK(!SyncRules::isIgnoredPath(builtin, QStringLiteral("tmp"), false), "G8 名字里含 tmp 但不是 *.tmp 的不误伤");

        // 目录专属规则
        CHECK(SyncRules::isIgnoredPath(QStringList{QStringLiteral("build/")},
                                       QStringLiteral("build"), true),
              "G9 `build/` 命中目录");
        CHECK(!SyncRules::isIgnoredPath(QStringList{QStringLiteral("build/")},
                                        QStringLiteral("build"), false),
              "G10 `build/` 不命中同名文件");

        // 含 '/' 的模式按整条路径匹配（只命中该层）
        const QStringList layered{QStringLiteral("src/*.o")};
        CHECK(SyncRules::isIgnoredPath(layered, QStringLiteral("src/main.o"), false), "G11 src/*.o 命中 src 下一层");
        CHECK(!SyncRules::isIgnoredPath(layered, QStringLiteral("src/sub/main.o"), false),
              "G12 `*` 不跨 '/'：src/sub/main.o 不命中");
        CHECK(!SyncRules::isIgnoredPath(layered, QStringLiteral("main.o"), false),
              "G13 带 '/' 的模式不匹配无目录的路径");

        // 通配符细节
        CHECK(SyncRules::matches(QStringLiteral("a?c"), QStringLiteral("abc"), false), "G14 `?` 匹配单字符");
        CHECK(!SyncRules::matches(QStringLiteral("a?c"), QStringLiteral("abbc"), false), "G15 `?` 只匹配一个字符");
        CHECK(SyncRules::matches(QStringLiteral("f[0-9].log"), QStringLiteral("f7.log"), false), "G16 字符区间");
        CHECK(!SyncRules::matches(QStringLiteral("f[!0-9].log"), QStringLiteral("f7.log"), false), "G17 取反字符类");
        CHECK(SyncRules::matches(QStringLiteral("*.tar.gz"), QStringLiteral("x.tar.gz"), false), "G18 多段后缀");

        // '!' 取反 + last-match-wins
        const QStringList negate{QStringLiteral("*.tmp"), QStringLiteral("!keep.tmp")};
        CHECK(!SyncRules::isIgnoredPath(negate, QStringLiteral("keep.tmp"), false),
              "G19 `!` 取反生效（后匹配者胜）");
        CHECK(SyncRules::isIgnoredPath(negate, QStringLiteral("other.tmp"), false),
              "G20 取反只影响被点名的那个");

        // 注释与空行
        const QStringList withComment{QStringLiteral("# 注释"), QString(), QStringLiteral("*.bak")};
        CHECK(SyncRules::isIgnoredPath(withComment, QStringLiteral("x.bak"), false), "G21 注释/空行被跳过");
        CHECK(!SyncRules::isIgnoredPath(withComment, QStringLiteral("# 注释"), false), "G22 注释本身不是规则");
    }
    {
        // .cvignore 真实读取（落盘到临时目录）
        const QString root = QDir::tempPath() + QStringLiteral("/cv_sync_ignore_test");
        QDir().mkpath(root);
        QFile f(root + QStringLiteral("/.cvignore"));
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Text), "G23 可写 .cvignore");
        f.write("# comment\n\n*.iso\nmydata/\n");
        f.close();

        const QStringList pats = SyncRules::loadUserPatterns(root);
        CHECK(pats.size() == 2 && pats[0] == QStringLiteral("*.iso"), "G24 读取时剥掉注释与空行");

        const QStringList eff = SyncRules::effectivePatterns(root);
        CHECK(eff.contains(QStringLiteral("*.iso"))
                  && eff.contains(QStringLiteral("Thumbs.db")),
              "G25 effectivePatterns = 内置 + 用户规则");
        CHECK(SyncRules::isIgnoredPath(eff, QStringLiteral("a/b/x.iso"), false), "G26 用户规则生效");
        CHECK(SyncRules::isIgnoredPath(eff, QStringLiteral("mydata"), true), "G27 用户目录规则生效");

        QDir(root).removeRecursively();
    }

    // ==================================================================
    std::printf("== H. 选择性同步（FR-3-7：只拦下载，且绝不删数据）==\n");
    {
        // ---- H1 云端新增、但在被排除目录下 → 不落地本地 ----
        SyncPlanInput in;
        in.now        = kNow;
        in.deviceName = QStringLiteral("PC-01");
        in.excludeDirs = QStringList{QStringLiteral("docs")};
        addFile(in.remote, QStringLiteral("docs/secret.txt"), 10, QStringLiteral("h1"), kNew);
        addFile(in.remote, QStringLiteral("keep.txt"), 10, QStringLiteral("h2"), kNew);
        SyncPlanResult r = SyncPlan::build(in);
        CHECK(kindIs(r, QStringLiteral("docs/secret.txt"), SyncAction::Skip),
              "H1 被排除目录下的云端新文件 → 跳过（不落地本地）");
        CHECK(kindIs(r, QStringLiteral("keep.txt"), SyncAction::DownloadNew),
              "H2 同批次里未被排除的文件照常下载");
        CHECK(findAct(r, QStringLiteral("docs/secret.txt"))->reason.contains(QStringLiteral("选择性")),
              "H3 跳过原因写明了是「选择性同步」排除，便于排查");

        // ---- H4 被排除的目录本身 ----
        in = SyncPlanInput();
        in.now = kNow; in.deviceName = QStringLiteral("PC-01");
        in.excludeDirs = QStringList{QStringLiteral("docs")};
        addDir(in.remote, QStringLiteral("docs"));
        r = SyncPlan::build(in);
        CHECK(kindIs(r, QStringLiteral("docs"), SyncAction::Skip),
              "H4 被排除目录本身不建本地目录");

        // ---- H5 边界：排除 docs 不得误伤 docs2 / docs.txt ----
        in = SyncPlanInput();
        in.now = kNow; in.deviceName = QStringLiteral("PC-01");
        in.excludeDirs = QStringList{QStringLiteral("docs")};
        addFile(in.remote, QStringLiteral("docs2/a.txt"), 1, QStringLiteral("b1"), kNew);
        addFile(in.remote, QStringLiteral("docs.txt"), 1, QStringLiteral("b2"), kNew);
        r = SyncPlan::build(in);
        CHECK(kindIs(r, QStringLiteral("docs2/a.txt"), SyncAction::DownloadNew),
              "H5 【边界】排除 docs 不应误伤 docs2/（前缀匹配必须带 '/'）");
        CHECK(kindIs(r, QStringLiteral("docs.txt"), SyncAction::DownloadNew),
              "H6 【边界】排除 docs 不应误伤 docs.txt");
        CHECK(SyncPlan::isExcluded(QStringList{QStringLiteral("docs")}, QStringLiteral("docs")),
              "H7 isExcluded：目录自身命中");
        CHECK(SyncPlan::isExcluded(QStringList{QStringLiteral("docs")}, QStringLiteral("docs/x")),
              "H8 isExcluded：子孙命中");
        CHECK(SyncPlan::isExcluded(QStringList{QStringLiteral("docs")}, QStringLiteral("docs")),
              "H10 isExcluded 幂等");

        // ---- H11 条目归一化：允许写 /docs/ 或带空格 ----
        in = SyncPlanInput();
        in.now = kNow; in.deviceName = QStringLiteral("PC-01");
        in.excludeDirs = QStringList{QStringLiteral(" /docs/ "), QStringLiteral("a/b/")};
        addFile(in.remote, QStringLiteral("docs/x.txt"), 1, QStringLiteral("n1"), kNew);
        addFile(in.remote, QStringLiteral("a/b/c.txt"), 1, QStringLiteral("n2"), kNew);
        addFile(in.remote, QStringLiteral("a/bc.txt"), 1, QStringLiteral("n3"), kNew);
        r = SyncPlan::build(in);
        CHECK(kindIs(r, QStringLiteral("docs/x.txt"), SyncAction::Skip),
              "H11 条目 /docs/ 会被归一化为 docs 并生效");
        CHECK(kindIs(r, QStringLiteral("a/b/c.txt"), SyncAction::Skip),
              "H12 嵌套排除 a/b/ 生效");
        CHECK(kindIs(r, QStringLiteral("a/bc.txt"), SyncAction::DownloadNew),
              "H13 【边界】排除 a/b 不应误伤 a/bc.txt");

        // ---- H14 空条目不得变成"排除一切" ----
        in = SyncPlanInput();
        in.now = kNow; in.deviceName = QStringLiteral("PC-01");
        in.excludeDirs = QStringList{QString(), QStringLiteral("  ")};
        addFile(in.remote, QStringLiteral("any.txt"), 1, QStringLiteral("e1"), kNew);
        r = SyncPlan::build(in);
        CHECK(kindIs(r, QStringLiteral("any.txt"), SyncAction::DownloadNew),
              "H14 【安全】空条目被忽略 —— 绝不能把空白行理解成「排除所有目录」");

        // ---- H15 上传方向不受影响 ----
        in = SyncPlanInput();
        in.now = kNow; in.deviceName = QStringLiteral("PC-01");
        in.excludeDirs = QStringList{QStringLiteral("docs")};
        addFile(in.local, QStringLiteral("docs/mine.txt"), 1, QStringLiteral("u1"), kNew);
        addFile(in.local, QStringLiteral("up.txt"), 1, QStringLiteral("u2"), kNew);
        r = SyncPlan::build(in);
        CHECK(kindIs(r, QStringLiteral("docs/mine.txt"), SyncAction::UploadNew),
              "H15 排除的是「云端→本地」，本地上传方向不受影响");

        // ---- H16 云端删了被排除目录里的文件 → 不删本地残留 ----
        in = SyncPlanInput();
        in.now = kNow; in.deviceName = QStringLiteral("PC-01");
        in.excludeDirs = QStringList{QStringLiteral("docs")};
        addFile(in.lastSynced, QStringLiteral("docs/old.txt"), 5, QStringLiteral("d1"), kOld);
        addFile(in.local, QStringLiteral("docs/old.txt"), 5, QStringLiteral("d1"), kOld);
        // remote 里没有它 → 云端已删
        r = SyncPlan::build(in);
        CHECK(kindIs(r, QStringLiteral("docs/old.txt"), SyncAction::Skip),
              "H16 【安全】云端删了被排除目录里的文件，也不去删本地残留副本");

        // ---- H17 本地删了被排除目录里的文件 → 不删云端 ----
        in = SyncPlanInput();
        in.now = kNow; in.deviceName = QStringLiteral("PC-01");
        in.excludeDirs = QStringList{QStringLiteral("docs")};
        addFile(in.lastSynced, QStringLiteral("docs/keep.txt"), 5, QStringLiteral("d2"), kOld);
        addFile(in.remote, QStringLiteral("docs/keep.txt"), 5, QStringLiteral("d2"), kOld);
        r = SyncPlan::build(in);
        CHECK(kindIs(r, QStringLiteral("docs/keep.txt"), SyncAction::Skip),
              "H17 【安全】本地删了被排除目录里的文件，也不去删云端（用户要的就是「云端保留」）");

        // ---- H18 冲突落在被排除目录 → 不偷偷下载 ----
        in = SyncPlanInput();
        in.now = kNow; in.deviceName = QStringLiteral("PC-01");
        in.excludeDirs = QStringList{QStringLiteral("docs")};
        addFile(in.lastSynced, QStringLiteral("docs/c.txt"), 5, QStringLiteral("base"), kOld);
        addFile(in.local, QStringLiteral("docs/c.txt"), 5, QStringLiteral("loc"), kOld);
        addFile(in.remote, QStringLiteral("docs/c.txt"), 9, QStringLiteral("srv"), kNew);
        r = SyncPlan::build(in);
        CHECK(kindIs(r, QStringLiteral("docs/c.txt"), SyncAction::Skip),
              "H18 被排除目录里的冲突 → 跳过，不得借「冲突处理」之名把云端内容拉下来");

        // ---- H19 未配置排除时行为完全不变（回归）----
        in = SyncPlanInput();
        in.now = kNow; in.deviceName = QStringLiteral("PC-01");
        addFile(in.remote, QStringLiteral("docs/x.txt"), 1, QStringLiteral("z1"), kNew);
        r = SyncPlan::build(in);
        CHECK(kindIs(r, QStringLiteral("docs/x.txt"), SyncAction::DownloadNew),
              "H19 未配置排除时照旧下载（默认行为未变）");
    }

    std::printf("\n结果：pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
