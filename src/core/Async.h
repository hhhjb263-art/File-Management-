/****************************************************************************
 * core/Async.h —— 把耗时操作丢进线程池，结果回到主线程再更新界面
 *
 * 用法：
 *   Async::run(this,
 *              [=] { return backend->listFolder(id); },
 *              [=](Result<QVector<FileItem>> r) { /* 主线程 *\/ });
 ****************************************************************************/
#pragma once

#include <QCoreApplication>
#include <QFutureWatcher>
#include <QObject>
#include <QtConcurrent>
#include <functional>
#include <type_traits>
#include <utility>

namespace cv {

class Async
{
public:
    // 带返回值：task 在线程池执行，then 在 owner 所在线程执行
    template <typename Task, typename Then>
    static void run(QObject *owner, Task &&task, Then &&then)
    {
        using T       = std::invoke_result_t<Task>;
        QObject *ctx  = owner ? owner : QCoreApplication::instance();
        auto *watcher = new QFutureWatcher<T>(ctx);

        QObject::connect(watcher, &QFutureWatcherBase::finished, ctx,
                         [watcher, then = std::forward<Then>(then)]() mutable {
                             T value = watcher->result();
                             watcher->deleteLater();
                             then(std::move(value));
                         });

        watcher->setFuture(QtConcurrent::run(std::forward<Task>(task)));
    }

    // 不需要返回值的后台任务
    static void run(std::function<void()> task)
    {
        QtConcurrent::run(std::move(task));
    }
};

} // namespace cv
