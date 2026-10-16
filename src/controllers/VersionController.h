/****************************************************************************
 * controllers/VersionController.h —— 文件历史版本控制器
 *
 * 对接服务端（server/src/app/main.cpp）：
 *   GET    /api/v1/files/:id/versions              列表（含合成的"当前版本"）
 *   POST   /api/v1/files/:id/versions              手动打一个快照
 *   POST   /api/v1/files/:id/versions/:vid/restore 回滚
 *   DELETE /api/v1/files/:id/versions/:vid         删掉某个历史版本
 *
 * 语义（重要，决定了界面上怎么展示）：
 *   · 服务端返回的第一条是**合成的"当前版本"**（current=true，id="0"），它不是真实快照行；
 *     所以"回滚到当前版本"没有任何意义，界面必须把它的回滚按钮禁掉。
 *   · 每次覆盖上传 / 回滚之前，服务端都会自动备份旧内容 —— 用户在界面上应该被明确告知
 *     "回滚不会弄丢现在这版"，否则没人敢点。
 *
 * 线程模型：同 TrashController —— Async::run + seq 丢弃乱序回包。
 ****************************************************************************/
#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QVector>

#include "core/Types.h"

namespace cv {

class Backend;
class VersionModel;

class VersionController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool    supported      READ supported      CONSTANT)
    Q_PROPERTY(int     count          READ count          NOTIFY versionsChanged)
    Q_PROPERTY(bool    busy           READ busy           NOTIFY busyChanged)
    Q_PROPERTY(QString fileId         READ fileId         NOTIFY targetChanged)
    Q_PROPERTY(QString fileName       READ fileName       NOTIFY targetChanged)
    Q_PROPERTY(int     currentVersion READ currentVersion NOTIFY versionsChanged)
    Q_PROPERTY(bool    hasTarget      READ hasTarget      NOTIFY targetChanged)
    Q_PROPERTY(QString emptyText      READ emptyText      CONSTANT)

public:
    explicit VersionController(Backend *backend, VersionModel *model, QObject *parent = nullptr);

    bool    supported() const { return m_backend != nullptr; }
    int     count() const { return m_versions.size(); }
    bool    busy() const { return m_busy; }
    QString fileId() const { return m_fileId; }
    QString fileName() const { return m_fileName; }
    int     currentVersion() const { return m_currentVersion; }
    bool    hasTarget() const { return !m_fileId.isEmpty(); }
    QString emptyText() const;

    // 打开某文件的版本历史（文件页选中文件后调用）
    Q_INVOKABLE void open(const QString &fileId, const QString &fileName);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void snapshot();  // 把当前内容存成一个历史版本
    Q_INVOKABLE void restore(const QString &versionId);
    Q_INVOKABLE void removeVersion(const QString &versionId);
    Q_INVOKABLE QVariantMap at(int i) const;
    Q_INVOKABLE void close();  // 清空当前目标（离开版本页时调用）

signals:
    void versionsChanged();
    void busyChanged();
    void targetChanged();
    void errorOccurred(const QString &message);
    void statusMessage(const QString &message, bool ok);
    void logMessage(const QString &level, const QString &text);
    // 回滚成功：文件内容变了，上层须刷新文件列表与预览
    void versionRestored(const QString &fileId);

private:
    void        setBusy(bool on);
    void        applyVersions(const QVector<FileVersion> &versions);
    static QString friendlyError(const QString &raw);

    Backend      *m_backend = nullptr;
    VersionModel *m_model   = nullptr;
    QVector<FileVersion> m_versions;
    QString m_fileId;
    QString m_fileName;
    int     m_currentVersion = 1;
    bool    m_busy = false;
    int     m_seq  = 0; // 序号：丢弃乱序回包
};

} // namespace cv
