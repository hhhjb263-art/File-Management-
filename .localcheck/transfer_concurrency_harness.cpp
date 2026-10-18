// 文件级并发 harness —— 验证 TransferManager 多任务并行、且不丢/不重复。
//
// 为什么必须要它：TransferManager 是 1190 行的状态机，本轮把 `Task m_cur`
// 改成了「指针 + 在途任务表」来实现文件级并发。编译通过**完全不能**说明它对：
//   · 回调按 gen 定位任务，找错任务 ⇒ 写成别的文件
//   · finishTask 后 m_cur 失效 ⇒ use-after-free
//   · 同一任务被结束两次 ⇒ 计数翻倍、UI 行状态错乱
// 这些只有真跑起来才暴露。
//
// 观测手法（不改生产代码、只用现成信号）：
//   1) enqueue 之后**立刻暂停** —— pauseCurrent 会对每个在途任务发一次 taskPaused(true)，
//      于是「暂停事件数」= 当时真正并行的任务数。
//   2) 恢复后等全部完成，断言：任务数齐、全成功、没有任何任务被结束两次。
//
// 编译（Windows mingw，与 sync_engine_harness 同款；运行时 Qt bin 要在 PATH 里）：
//   g++ -std=gnu++1z -static -static-libgcc -static-libstdc++ -fexceptions -mthreads \
//       -DUNICODE -D_UNICODE -DWIN32 -DQT_NO_DEBUG \
//       -I src -I src/core -I src/net -I src/data -I src/transfer \
//       -I "$QT/include" -I "$QT/include/QtCore" -I "$QT/include/QtSql" \
//       -I "$QT/include/QtNetwork" -I "$QT/include/QtGui" \
//       .localcheck/transfer_concurrency_harness.cpp src/transfer/TransferManager.cpp \
//       src/net/MockBackend.cpp src/net/Json.cpp \
//       src/core/AppPaths.cpp src/core/Settings.cpp src/core/Util.cpp \
//       src/core/Logging.cpp src/core/Crypto.cpp \
//       "$QT/lib/libQt6Sql.a" "$QT/lib/libQt6Network.a" "$QT/lib/libQt6Core.a" \
//       -lmingw32 -lws2_32 -lversion -lwinmm -lole32 -luuid -lbcrypt -luserenv -lshell32 \
//       -o transfer_harness.exe
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>

#include "net/MockBackend.h"
#include "transfer/TransferManager.h"

using namespace cv;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg)                        \
    do {                                        \
        if (cond) {                             \
            ++g_pass;                           \
        } else {                                \
            ++g_fail;                           \
            std::printf("  FAIL: %s\n", msg);   \
        }                                       \
    } while (0)

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);   // 重定向到文件时也不丢输出
    QCoreApplication app(argc, argv);

    constexpr int kFiles = 12;
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::printf("无法创建临时目录\n");
        return 2;
    }
    QStringList paths;
    for (int i = 0; i < kFiles; ++i) {
        const QString p = tmp.path() + QStringLiteral("/f%1.bin").arg(i, 2, 10, QLatin1Char('0'));
        QFile f(p);
        if (!f.open(QIODevice::WriteOnly)) {
            std::printf("无法创建测试文件 %s\n", qPrintable(p));
            return 2;
        }
        f.write(QByteArray(200 * 1024, char('A' + (i % 26))));   // 200KB，各内容不同 ⇒ 不触发秒传
        f.close();
        paths << p;
    }

    MockBackend    be;
    TransferManager tm(&be);

    QSet<QString> finished;
    int           finishedOk = 0;
    int           dupFinish  = 0;
    QObject::connect(&tm, &TransferManager::taskFinished,
                     [&](const QString &id, bool ok, const QString &msg) {
                         if (finished.contains(id))
                             ++dupFinish;
                         finished.insert(id);
                         if (ok)
                             ++finishedOk;
                         else
                             std::printf("  [失败] %s: %s\n", qPrintable(id), qPrintable(msg));
                     });
    // 同名冲突：自动选「覆盖」，否则任务会停在等用户回答上、harness 永远等不到结束
    QObject::connect(&tm, &TransferManager::conflictDetected, [&](const QString &id,
                                                                  const QString &,
                                                                  const QString &) {
        tm.resolveConflict(id, true);
    });

    QObject::connect(&tm, &TransferManager::logMessage,
                     [&](const QString &, const QString &text) {
                         if (text.contains(QStringLiteral("upload_id=")) ||
                             text.startsWith(QStringLiteral("init")))
                             std::printf("  LOG: %s\n", qPrintable(text));
                     });

    int pausedCount = 0;
    QObject::connect(&tm, &TransferManager::taskPaused, [&](const QString &, bool paused) {
        if (paused)
            ++pausedCount;
    });

    // ---- 1) 并发度 ----
    tm.enqueueUpload(paths, QString());
    tm.pauseCurrent();                    // 每个在途任务会发一次 taskPaused(true)
    const int concurrent = pausedCount;
    std::printf("在途并发数 = %d（kFileConcurrency 期望 4）\n", concurrent);
    CHECK(concurrent >= 2, "文件级并发应生效（在途任务 ≥2）");
    CHECK(concurrent <= 4, "在途任务不应超过 4");
    tm.resumeCurrent();

    // ---- 2) 等全部结束 ----
    QEventLoop loop;
    QTimer     timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&tm, &TransferManager::taskFinished, [&]() {
        if (finished.size() >= kFiles)
            loop.quit();
    });
    timeout.start(60000);
    loop.exec();

    std::printf("结束 %d / %d（成功 %d，重复结束 %d）\n", int(finished.size()), kFiles, finishedOk,
                dupFinish);
    CHECK(finished.size() == kFiles, "所有任务都应结束（不多不少）");
    CHECK(finishedOk == kFiles, "所有任务都应成功");
    CHECK(dupFinish == 0, "同一任务不应被结束两次");

    // ---- 3) 同名冲突：UI 的"覆盖"应答必须真的能解掉冲突（回归用例）----
    // 盯的是一个真实回归：resolveConflict 曾用 m_cur 做守卫，而它是从**UI 回调**进来的
    // （那时 m_cur 已被别的任务改掉或置空）⇒ 应答被当成"无对应冲突"丢弃
    // ⇒ 任务永久停在 awaitingConflict，一直占着并发槽 ⇒ 后续文件排队/超时。
    std::printf("---- 冲突应答用例 ----\n");
    {
        QTemporaryDir tmp2;
        const QString p2 = tmp2.path() + QStringLiteral("/f00.bin");   // 与第一批**同名**
        QFile f(p2);
        f.open(QIODevice::WriteOnly);
        f.write(QByteArray(4 * 1024, 'Z'));                            // 内容不同 ⇒ 撞 409
        f.close();

        const int before     = finished.size();
        const int beforeOk   = finishedOk;
        tm.enqueueUpload(QStringList{p2}, QString());

        QEventLoop l2;
        QTimer     t2;
        t2.setSingleShot(true);
        QObject::connect(&tm, &TransferManager::taskFinished, &l2, &QEventLoop::quit);
        QObject::connect(&t2, &QTimer::timeout, &l2, &QEventLoop::quit);
        t2.start(20000);
        l2.exec();

        CHECK(finished.size() == before + 1, "冲突任务必须结束（不能应答被丢弃后永久卡住）");
        CHECK(finishedOk >= beforeOk + 1, "冲突任务应成功（覆盖）");
        std::printf("  冲突用例：结束 %d，成功增量 %d\n", int(finished.size() - before),
                    finishedOk - beforeOk);
    }

    std::printf("\n结果：pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
