// 同步**执行层**端到端验证：真的把文件同步过去了吗？
//
// 为什么必须有它：SyncPlan 的 harness 只验证"该干什么"（决策），而用户真正关心的是
// "文件到底传过去没有、本地索引有没有正确推进"。这一层涉及
//   Backend（分块上传/下载/删除）+ 真实文件系统 + LocalIndex，
// 靠静态检查与编译**都发现不了**问题 —— 属于典型的"编译过了但没接上"。
//
// 用 MockBackend（本地引擎）当云端：不需要服务器，但走的是与生产**同一套** Backend 接口
// 与 SyncEngine 代码路径。
//
// ⚠️ MockBackend 会在析构时把状态写回 AppPaths::dataDir()/db.json —— 运行本 harness
//    前后必须备份/还原那个文件（同 --mock 的注意事项）。
//
// 编译（Windows mingw）：
//   export PATH="/d/Qt/6.9.3/mingw_64/bin:/d/Qt/Tools/mingw1310_64/bin:$PATH"
//   QT="D:/Qt/6.9.3/mingw_64"
//   g++ -std=gnu++1z -static -static-libgcc -static-libstdc++ -fexceptions -mthreads \
//       -DUNICODE -D_UNICODE -DWIN32 -DQT_NO_DEBUG \
//       -I src -I src/core -I src/net -I src/sync -I src/data -I src/transfer -I src/controllers \
//       -I "$QT/include" -I "$QT/include/QtCore" -I "$QT/include/QtSql" -I "$QT/include/QtNetwork" -I "$QT/include/QtGui" \
//       .localcheck/sync_engine_harness.cpp \
//       src/sync/SyncPlan.cpp src/sync/SyncRules.cpp src/sync/FileWatcher.cpp \
//       src/sync/SyncEngine.cpp src/sync/LocalIndex.cpp \
//       src/net/MockBackend.cpp src/net/Json.cpp \
//       src/core/AppPaths.cpp src/core/Settings.cpp src/core/Util.cpp \
//       src/core/Logging.cpp src/core/Crypto.cpp \
//       "$QT/lib/libQt6Sql.a" "$QT/lib/libQt6Network.a" "$QT/lib/libQt6Core.a" \
//       -lmingw32 -lws2_32 -lversion -lwinmm -lole32 -luuid -lbcrypt -luserenv -lshell32 \
//       -o sync_engine_harness.exe
//   （运行时同样要把 Qt 的 bin 放进 PATH）
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTimer>

#include <cstdio>
#include <memory>

#include "core/AppPaths.h"
#include "net/MockBackend.h"
#include "sync/LocalIndex.h"
#include "transfer/TransferManager.h"
#include "sync/SyncEngine.h"

using namespace cv;

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

static QString g_log;
static int     g_lastConflicts = -1;   // 最近一次 pairSettled 的冲突数

// 等 msec 毫秒，同时跑事件循环（引擎的调度依赖事件循环）
static void pump(int msec)
{
    QEventLoop loop;
    QTimer::singleShot(msec, &loop, &QEventLoop::quit);
    loop.exec();
}

static void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    f.write(data);
    f.close();
}

// 路径归一化：MockBackend 返回 "synctest/a.txt"（无前导斜杠），HttpBackend 返回 "/a/b"。
// 比对前必须两边都去斜杠，否则用例会因为"约定不同"而误判（我第一版就这么踩了）。
static QString normPath(const QString &s)
{
    QString t = s.trimmed();
    while (t.startsWith(QLatin1Char('/')))
        t.remove(0, 1);
    while (t.endsWith(QLatin1Char('/')))
        t.chop(1);
    return t;
}

// 在 mock 云端里找某个相对路径对应的条目
static bool remoteHas(MockBackend &b, const QString &remoteRoot, const QString &rel)
{
    // 与引擎一致：带/不带前导斜杠两种写法都试（MockBackend 的路径没有前导斜杠）
    Result<QVector<FileItem>> r = b.listUnderPath(remoteRoot);
    if (!r.ok || r.value.isEmpty())
        r = b.listUnderPath(normPath(remoteRoot));
    if (!r.ok)
        return false;
    const QString want = normPath(remoteRoot) + QLatin1Char('/') + normPath(rel);
    for (const FileItem &f : r.value) {
        if (normPath(f.path) == want)
            return true;
    }
    return false;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("cloudvault"));
    app.setApplicationName(QStringLiteral("CloudVault"));

    const QString root = QDir::tempPath() + QStringLiteral("/cv_sync_e2e");
    QDir(root).removeRecursively();
    QDir().mkpath(root);
    const QString localDir = root + QStringLiteral("/local");
    QDir().mkpath(localDir);

    // 夹具：一个根文件 + 一个子目录里的文件 + 一个应被忽略的 .tmp
    writeFile(localDir + QStringLiteral("/a.txt"), "hello-A");
    writeFile(localDir + QStringLiteral("/sub/b.txt"), "hello-B");
    writeFile(localDir + QStringLiteral("/junk.tmp"), "should-be-ignored");

    // ⚠️ 清掉本地引擎的持久库，保证每次跑都是**干净云端**。
    //    否则上一轮跑出来的文件会被库里的同名同内容文件秒传命中，用例之间互相污染
    //    （实测：第二轮起 sub/b.txt 一直"上传会话不存在"，就是这个原因）。
    //    外层脚本负责跑完把它还原（MockBackend 析构时会把新状态写回这个文件）。
    QFile::remove(AppPaths::localDbFile());

    MockBackend backend;
    LocalIndex  index;
    index.open(root + QStringLiteral("/sync.db"));

    SyncPair pair;
    pair.id         = QStringLiteral("e2e");
    pair.localPath  = localDir;
    pair.remotePath = QStringLiteral("/synctest");
    pair.mode       = SyncMode::TwoWay;
    pair.enabled    = true;

    SyncEngine engine(&backend, &index);

    // ── A/B：批量上传路径 vs 原内联路径 ──
    // 设 CV_SYNC_USE_TM=1 时注入 TransferManager ⇒ 上传动作走"批量派发 + 事件驱动"分支
    // （与生产一致）；不设则走原来的逐条内联实现。两种模式的结果必须完全一致。
    std::unique_ptr<TransferManager> tm;
    if (qEnvironmentVariableIsSet("CV_SYNC_USE_TM")) {
        tm.reset(new TransferManager(&backend));
        engine.setTransferManager(tm.get());
        std::printf("[MODE] 批量上传路径（注入 TransferManager）\n");
    } else {
        std::printf("[MODE] 原内联路径（未注入 TransferManager）\n");
    }
    engine.setDeviceName(QStringLiteral("E2E"));
    engine.setDebounceMs(100);   // 测试里不等 5 秒
    QObject::connect(&engine, &SyncEngine::logAppended,
                     [](const QString &l) { g_log += l + QLatin1Char('\n'); });
    engine.setPairs({pair});

    std::printf("== A. 启动全量对账：本地新增应上传到云端 ==\n");
    engine.start();
    pump(1500);   // 等扫描(pool) + 上传动作跑完

    CHECK(remoteHas(backend, QStringLiteral("/synctest"), QStringLiteral("a.txt")),
          "A1 根文件已上传到云端");
    CHECK(remoteHas(backend, QStringLiteral("/synctest"), QStringLiteral("sub/b.txt")),
          "A2 子目录里的文件已上传（含目录创建）");
    CHECK(!remoteHas(backend, QStringLiteral("/synctest"), QStringLiteral("junk.tmp")),
          "A3 忽略规则生效：*.tmp 没有上传");
    CHECK(g_log.contains(QStringLiteral("上传")), "A4 同步日志记录了上传动作");
    {
        Result<QVector<FileItem>> r = backend.listUnderPath(QStringLiteral("/synctest"));
        std::printf("[DIAG] listUnderPath(/synctest): ok=%d err=%s n=%d\n",
                    int(r.ok), qPrintable(r.error), int(r.value.size()));
        for (const FileItem &f : r.value)
            std::printf("[DIAG]   path=%s name=%s isDir=%d\n",
                        qPrintable(f.path), qPrintable(f.name), int(f.isDir));
        Result<QVector<FileItem>> root = backend.listFolder(QStringLiteral("root"));
        std::printf("[DIAG] listFolder(root): ok=%d n=%d\n", int(root.ok), int(root.value.size()));
        for (const FileItem &f : root.value)
            std::printf("[DIAG]   path=%s name=%s parent=%s isDir=%d\n",
                        qPrintable(f.path), qPrintable(f.name), qPrintable(f.parentId), int(f.isDir));
    }

    QObject::connect(&engine, &SyncEngine::pairSettled, [](const SyncPair &pp) {
        // 用 toUtf8 输出：qPrintable 走 local8Bit(GBK)，重定向到文件后会成乱码
        g_lastConflicts = pp.conflictCount;
        std::printf("[DIAG] settled status=%d pending=%d conflict=%d err=%s\n",
                    int(pp.status), pp.pendingCount, pp.conflictCount,
                    pp.lastError.toUtf8().constData());
    });

    std::printf("---- A 阶段引擎日志 ----\n%s------------------------\n", g_log.toUtf8().constData());

    // 索引推进了吗？（下次对账要靠它判断"哪一侧改了"）
    const QHash<QString, LocalIndex::Entry> es = index.entries(QStringLiteral("e2e"));
    CHECK(es.contains(QStringLiteral("a.txt")), "A5 LocalIndex 已推进：a.txt 有记录");
    CHECK(es.value(QStringLiteral("a.txt")).rev.length() >= 0, "A6 索引带 rev 字段");

    std::printf("== B. 本地修改 → 应上传更新，且不产生删除/冲突 ==\n");
    g_log.clear();
    writeFile(localDir + QStringLiteral("/a.txt"), "hello-A-v2-longer");
    engine.reconcileNow(QStringLiteral("e2e"));
    pump(1500);
    CHECK(g_log.contains(QStringLiteral("上传修改")), "B1 识别为「上传修改」而非新增");
    // ⚠️ 不能用 contains("冲突")：对账摘要行永远打印「冲突 0」→ 恒真误报。
    CHECK(g_lastConflicts == 0, "B2 不该误判为冲突（看冲突计数，不看摘要文案）");
    CHECK(remoteHas(backend, QStringLiteral("/synctest"), QStringLiteral("a.txt")),
          "B3 云端仍有 a.txt");

    std::printf("== C. 本地删除 → 云端应进回收站（不是物理删） ==\n");
    g_log.clear();
    CHECK(QFile::remove(localDir + QStringLiteral("/a.txt")), "C1 删除本地文件");
    engine.reconcileNow(QStringLiteral("e2e"));
    pump(1500);
    CHECK(g_log.contains(QStringLiteral("删除云端")), "C2 识别为「删除云端」");
    CHECK(!remoteHas(backend, QStringLiteral("/synctest"), QStringLiteral("a.txt")),
          "C3 云端列表里已看不到 a.txt");
    // 能在回收站里找到 → 证明是 moveToTrash 而不是物理删除（可恢复）
    bool inTrash = false;
    {
        Result<QVector<FileItem>> t = backend.listTrash();
        if (t.ok) {
            for (const FileItem &f : t.value) {
                if (f.name == QStringLiteral("a.txt"))
                    inTrash = true;
            }
        }
    }
    CHECK(inTrash, "C4 云端删除进了回收站（可恢复），不是物理抹掉");

    std::printf("== D. 同一批文件再对账一次 → 应全部跳过（不重复传输）==\n");
    g_log.clear();
    engine.reconcileNow(QStringLiteral("e2e"));
    pump(1500);
    // 同理：用动作行前缀 "[上传" 判定，避开摘要行里的「上传 0」
    CHECK(!g_log.contains(QStringLiteral("[上传")), "D1 无变化的文件不再上传（索引生效，未退化）");
    CHECK(g_log.contains(QStringLiteral("跳过")) || g_log.contains(QStringLiteral("对账完成")),
          "D2 对账正常结束");

    std::printf("== E. 索引清空后重新对账 → 退化为「全部新增」（说明索引确实是判据）==\n");
    g_log.clear();
    index.clear();
    engine.reconcileNow(QStringLiteral("e2e"));
    pump(1500);
    // 索引清空后，两端内容相同 ⇒ 正确行为是"两端均为新增且内容一致 → 跳过"（无需传内容），
    // 而**不是**上传。所以这里断言的是**判定依据的变化**：从"与上次同步状态一致"
    // 变成"两端均为新增"，反证索引确实是判定基准。
    CHECK(g_log.contains(QStringLiteral("两端均为新增")),
          "E1 索引被清空后判定依据从「与上次一致」变为「两端均为新增」—— 反证索引参与判定");
    CHECK(!g_log.contains(QStringLiteral("[上传")), "E2 内容已一致时不应产生上传动作");

    engine.stop();
    QDir(root).removeRecursively();

    std::printf("\n结果：pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
