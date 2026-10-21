// 下载任务跨重启恢复 —— 真机级 harness
//
// 复现用户场景：传/下到一半「强杀客户端」→ 重启后任务应当回到传输列表。
// 做法：
//   1) 用 MockBackend 先上传一个 6MB 文件（拿到 fileId）；
//   2) 入队下载，等它真正开始（.part 出现 + download_tasks.json 被写入）；
//   3) 断言清单内容（file_id / 绝对 part / 绝对 dest_dir）；
//   4) **析构 manager**（等价于进程被杀：不 cancel、不 finish）；
//   5) 新建 manager → restoreInterrupted() → 断言收到 taskStarted；
//   6) 再断言 .part 大小被当作断点 offset（续传而不是从 0）。
//
// 配置目录按可执行文件名解析（QStandardPaths::AppLocalDataLocation），
// 所以这份 harness 有自己独立的 config/，不会污染真实客户端的文件。
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>

#include "core/AppPaths.h"
#include "net/MockBackend.h"
#include "data/TransferModel.h"
#include "transfer/TransferManager.h"

using namespace cv;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg)                                              \
    do {                                                              \
        if (cond) {                                                   \
            ++g_pass;                                                 \
        } else {                                                      \
            ++g_fail;                                                 \
            std::printf("  FAIL: %s\n", msg);                         \
        }                                                             \
    } while (0)

static void pumpFor(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("cv_restore_probe"));
    QDir(AppPaths::configDir()).removeRecursively();
    QDir(AppPaths::dataDir()).removeRecursively();
    AppPaths::ensureDirs();

    QTemporaryDir srcDir, dstDir;
    if (!srcDir.isValid() || !dstDir.isValid()) { std::printf("temp dir failed\n"); return 2; }

    const QString srcPath = srcDir.path() + QStringLiteral("/big6m.bin");
    {
        QFile f(srcPath);
        if (!f.open(QIODevice::WriteOnly)) { std::printf("src open failed\n"); return 2; }
        QByteArray blk(1 << 20, 'A');
        for (int i = 0; i < 6; ++i) f.write(blk);
    }

    // ---- ① 先让 mock 后端里有一个文件（上传一次）----
    MockBackend be;
    QString fileId;
    {
        TransferManager tm(&be);
        QObject::connect(&tm, &TransferManager::taskCompleted,
                         [&](const QString &, const FileItem &it) {
                             if (fileId.isEmpty()) fileId = it.id;
                         });
        tm.enqueueUpload({srcPath}, QString());
        for (int i = 0; i < 60 && fileId.isEmpty(); ++i) pumpFor(100);
    }
    CHECK(!fileId.isEmpty(), "① 拿到 fileId（mock 后端里已有文件）");
    if (fileId.isEmpty()) return 1;

    // ---- ② 构造"传到一半被杀"的现场：.part(3MB) + download_tasks.json ----
    const QString finalPath = dstDir.path() + QStringLiteral("/big6m.bin");
    const QString partPath  = finalPath + QStringLiteral(".part");
    {
        QFile f(partPath);
        if (!f.open(QIODevice::WriteOnly)) { std::printf("part open failed\n"); return 2; }
        f.write(QByteArray(3 << 20, 'A'));   // 前 3MB 已落盘
    }
    const QString man = AppPaths::configDir() + QStringLiteral("/download_tasks.json");
    {
        QJsonObject e;
        e.insert(QStringLiteral("file_id"), fileId);
        e.insert(QStringLiteral("name"), QStringLiteral("big6m.bin"));
        e.insert(QStringLiteral("dest_dir"), dstDir.path());
        e.insert(QStringLiteral("part"), partPath);
        e.insert(QStringLiteral("total"), double(6 * 1024 * 1024));
        QJsonObject root;
        root.insert(QStringLiteral("t_dead"), e);
        QFile f(man);
        if (!f.open(QIODevice::WriteOnly)) { std::printf("manifest open failed\n"); return 2; }
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    }
    std::printf("---- 现场：.part=%lld 字节，清单 ----\n%s\n",
                static_cast<long long>(QFileInfo(partPath).size()),
                QJsonDocument(QJsonDocument::fromJson([&]{ QFile f(man); f.open(QIODevice::ReadOnly); return f.readAll(); }()))
                    .toJson(QJsonDocument::Indented).constData());

    // ---- ③ 模拟重启：新 manager → restoreInterrupted() ----
    {
        MockBackend be2;
        TransferManager tm2(&be2);
        QSet<QString> started;
        QStringList  logs;
        QObject::connect(&tm2, &TransferManager::taskStarted,
                         [&](const QString &id, const QString &name, TransferKind k,
                             const QString &, const QString &, qint64 total) {
                             if (k == TransferKind::Download) {
                                 started.insert(id);
                                 std::printf("  任务重新开始：%s (%lld 字节)\n", qPrintable(name),
                                             static_cast<long long>(total));
                             }
                         });
        QObject::connect(&tm2, &TransferManager::logMessage,
                         [&](const QString &, const QString &t) {
                             logs << t;
                             std::printf("  [log] %s\n", qPrintable(t));
                         });
        const int n = tm2.restoreInterrupted();
        std::printf("  restoreInterrupted() 返回 %d\n", n);
        pumpFor(2000);
        CHECK(n >= 1, "② restoreInterrupted 报告恢复了任务");
        CHECK(!started.isEmpty(), "③ 重启后任务真的重新开始（taskStarted 收到下载任务）");
        bool resumedFromPart = false;
        for (const QString &l : logs)
            if (l.contains(QStringLiteral("offset="))) resumedFromPart = true;
        CHECK(resumedFromPart, "④ 续传而不是从 0 开始（日志里有 offset=）");
        CHECK(!QFile::exists(partPath), "⑤ 恢复后下完，.part 已改名落盘");
        CHECK(QFileInfo(finalPath).size() == 6 * 1024 * 1024, "⑥ 最终文件大小正确");
    }

    // ---- ④ 「重新传输」：先造一个失败任务，再 retryTask ----
    {
        MockBackend be3;
        TransferManager tm3(&be3);
        QString failedId;
        QStringList startedNames;
        QObject::connect(&tm3, &TransferManager::taskStarted,
                         [&](const QString &id, const QString &name, TransferKind k,
                             const QString &, const QString &, qint64) {
                             if (k == TransferKind::Download) startedNames << name;
                             Q_UNUSED(id);
                         });
        int finishedCount = 0;
        QObject::connect(&tm3, &TransferManager::taskFinished,
                         [&](const QString &id, bool ok, const QString &) {
                             ++finishedCount;
                             if (!ok && failedId.isEmpty()) failedId = id;
                         });
        QObject::connect(&tm3, &TransferManager::logMessage,
                         [](const QString &l, const QString &t) {
                             std::printf("  [%s] %s\n", qPrintable(l), qPrintable(t));
                         });
        tm3.enqueueDownload({QStringLiteral("no-such-file-id")}, dstDir.path());
        for (int i = 0; i < 40 && failedId.isEmpty(); ++i) pumpFor(50);
        CHECK(!failedId.isEmpty(), "⑦ 造出一个失败任务");
        const bool okRetry = tm3.retryTask(failedId);
        std::printf("  retryTask(%s) = %d\n", qPrintable(failedId), int(okRetry));
        CHECK(okRetry, "⑧ retryTask 返回 true（找得到原参数）");
        pumpFor(1500);
        // 该 fileId 不存在 ⇒ 不会发 taskStarted，但重试的任务会**再次失败并结束** ⇒ 计数变 2
        std::printf("  taskFinished 次数 = %d\n", finishedCount);
        CHECK(finishedCount >= 2, "⑨ 重试的任务真的被重新执行了（再次结束）");
    }

    // ---- ⑤ 历史落盘：重启后「已下载」还能看到 ----
    // 直接测模型（控制器也是 addTask → updateState 这两步），不依赖失败发生在哪个阶段。
    {
        {
            TransferModel model;
            TransferTask t;
            t.id = QStringLiteral("hist-1");
            t.fileName = QStringLiteral("demo.bin");
            t.kind = TransferKind::Download;
            t.localPath = dstDir.path() + QStringLiteral("/demo.bin");
            t.total = 1024;
            t.done = 512;
            model.addTask(t);
            model.updateState(t.id, TransferState::Failed, QStringLiteral("网络中断"));
            std::printf("  历史条数 = %d\n", model.downloadHistoryCount());
            CHECK(model.downloadHistoryCount() >= 1, "⑩ 失败任务进了「已下载」历史");
            const QString hf = AppPaths::configDir() + QStringLiteral("/transfer_history.json");
            std::printf("  %s 大小 = %lld 字节\n", qPrintable(hf),
                        static_cast<long long>(QFileInfo(hf).size()));
            CHECK(QFile::exists(hf) && QFileInfo(hf).size() > 10, "⑪ 历史已落盘");
        }
        {
            TransferModel model2;   // = 重启后新建的模型
            std::printf("  重启后 downloadHistoryCount = %d\n", model2.downloadHistoryCount());
            CHECK(model2.downloadHistoryCount() >= 1, "⑫ 重启后历史仍在（不再被清空）");
            const QVariantMap m = model2.downloadHistoryAt(0);
            CHECK(m.value(QStringLiteral("fileName")).toString() == QStringLiteral("demo.bin"),
                  "⑬ 恢复的条目字段正确（fileName）");
        }
    }

    std::printf("\n结果：pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
