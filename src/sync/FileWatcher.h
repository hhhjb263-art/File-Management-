/****************************************************************************
 * sync/FileWatcher.h —— 目录递归监听 + 防抖（PRD FR-3-1）
 *
 * ⚠️ QFileSystemWatcher **不递归** —— 它只报"你登记过的那一个目录"本身的变化。
 *    所以本类必须把同步根下的**每个子目录**都登记进去，并在收到变更后重新走一遍
 *    目录树，把新建的子目录补登记、把消失的目录摘掉。
 *
 * 防抖（FR-3-1 的"5 秒防抖合并"）：一次大文件复制会触发成百上千次 directoryChanged，
 *   每次都去对账是灾难。策略是**每次变更重置定时器**，静默满 debounceMs 才发 settled()。
 *   于是"连续写一盘"只会在写完之后对账一次。
 *
 * 忽略目录**不登记**：既省句柄，也避免 .git/ 这种目录把事件刷爆。
 ****************************************************************************/
#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

class QFileSystemWatcher;
class QTimer;

namespace cv {

class FileWatcher : public QObject
{
    Q_OBJECT
public:
    explicit FileWatcher(QObject *parent = nullptr);

    // 防抖窗口，默认 5000ms（FR-3-1 的 5 秒）
    void setDebounceMs(int ms);
    int  debounceMs() const;

    // 登记一个同步根（绝对路径）。内部递归登记其下所有非忽略目录。
    void watch(const QString &rootAbs);
    void unwatchAll();

    // 自某次变更起，是否仍处在防抖窗口内（UI 可借此显示"待同步"）
    bool isPending() const;

    // 立即结束防抖窗口并发出 settled()（用于"手动立即同步"）
    void flush();

signals:
    void settled();                        // 防抖结束：可以开始一次对账
    void pendingChanged(bool pending);

private:
    void onDirectoryChanged(const QString &path);
    // 重新走一遍目录树：补登记新目录、摘掉消失的目录
    void resyncRegistration();
    void registerTree(const QString &absDir, QStringList &alive);
    QStringList ignorePatternsFor(const QString &rootAbs) const;

    QFileSystemWatcher *m_watcher = nullptr;
    QTimer             *m_debounce = nullptr;
    QStringList         m_roots;             // 绝对路径
    QStringList         m_registeredDirs;    // 当前已登记到 watcher 的目录（绝对路径）
    int                 m_debounceMs = 5000;
    bool                m_pending = false;
};

} // namespace cv
