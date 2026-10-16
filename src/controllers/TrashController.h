/****************************************************************************
 * controllers/TrashController.h —— 回收站控制器
 *
 * 对接服务端（server/src/app/main.cpp）：
 *   GET  /api/v1/trash          列表（顶层目录 + 顶层文件）
 *   POST /api/v1/trash          移入回收站（body: file_id 或 dir）
 *   POST /api/v1/trash/restore  恢复
 *   POST /api/v1/trash/purge    彻底删除（不可恢复）
 *   POST /api/v1/trash/empty    清空
 *
 * 线程模型：全部经 cv::Async::run 落到线程池执行，回调回主线程再动界面 ——
 * 与"HttpBackend 同步方法会阻塞"的约定配套，绝不重演主线程冻结。
 * 用 m_seq 序号丢弃乱序回包（连续点"刷新"时旧结果不许覆盖新结果）。
 ****************************************************************************/
#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QVector>

#include "core/Types.h"

namespace cv {

class Backend;
class FileListModel;

class TrashController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool    supported         READ supported         CONSTANT)
    Q_PROPERTY(QString unsupportedNotice READ unsupportedNotice CONSTANT)
    Q_PROPERTY(int     count             READ count             NOTIFY trashChanged)
    Q_PROPERTY(bool    busy              READ busy              NOTIFY busyChanged)
    Q_PROPERTY(qint64  usedBytes         READ usedBytes         NOTIFY trashChanged)
    Q_PROPERTY(QString usedText          READ usedText          NOTIFY trashChanged)
    Q_PROPERTY(QString emptyText         READ emptyText         CONSTANT)
    Q_PROPERTY(bool    hasDirEntries     READ hasDirEntries     NOTIFY trashChanged)

public:
    explicit TrashController(Backend *backend, FileListModel *model, QObject *parent = nullptr);

    bool    supported() const { return m_backend != nullptr; }
    QString unsupportedNotice() const { return QString(); }
    int     count() const { return m_items.size(); }
    bool    busy() const { return m_busy; }
    qint64  usedBytes() const { return m_usedBytes; }
    QString usedText() const;
    QString emptyText() const;
    bool    hasDirEntries() const;

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void restore(const QString &id);
    Q_INVOKABLE void purge(const QString &id);       // 彻底删除（危险，调用方须先二次确认）
    Q_INVOKABLE void emptyTrash();                   // 清空（危险）
    Q_INVOKABLE QVariantMap at(int i) const;

signals:
    void trashChanged();
    void busyChanged();
    void errorOccurred(const QString &message);
    void statusMessage(const QString &message, bool ok);
    void logMessage(const QString &level, const QString &text);
    void refreshFinished(bool ok);
    // 回收站内容发生变化（恢复 / 彻底删除 / 清空成功）—— 上层据此刷新文件列表与存储用量
    void trashMutated();

private:
    void        setBusy(bool on);
    void        applyItems(const QVector<FileItem> &items);
    static QString friendlyError(const QString &raw);

    Backend          *m_backend = nullptr;
    FileListModel    *m_model   = nullptr;
    QVector<FileItem> m_items;
    qint64 m_usedBytes = 0;
    bool   m_busy = false;
    int    m_seq  = 0; // 序号：丢弃乱序回包
};

} // namespace cv
