/****************************************************************************
 * controllers/TagController.h —— 标签控制器
 *
 * 对接服务端（server/src/app/main.cpp）：
 *   GET    /api/v1/tags                标签列表（带关联文件数）
 *   POST   /api/v1/tags                新建标签 {name,color}（同名 NOCASE 幂等）
 *   DELETE /api/v1/tags/:id            删除标签（服务端级联清理 file_tag）
 *   GET    /api/v1/files/:id/tags      某文件的标签
 *   PUT    /api/v1/files/:id/tags      全量设置某文件的标签 {tag_ids:[]}
 *   POST   /api/v1/search              检索 {keyword,tag_ids,type}
 *
 * 两件事在这里合并：
 *   ① 标签自身的增删查（TagsPage 的主列表）；
 *   ② "文件 ↔ 标签"的关联维护（文件页右键菜单里的标签勾选）。
 * 两者数据同源，拆成两个控制器只会让它们各自缓存一份标签列表然后互相打架。
 *
 * 线程模型：同 TrashController —— Async::run + seq 丢弃乱序回包。
 ****************************************************************************/
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include "core/Types.h"

namespace cv {

class Backend;
class TagModel;

class TagController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool    supported   READ supported   CONSTANT)
    Q_PROPERTY(int     count       READ count       NOTIFY tagsChanged)
    Q_PROPERTY(bool    busy        READ busy        NOTIFY busyChanged)
    Q_PROPERTY(QString filterTagId READ filterTagId NOTIFY filterChanged)
    Q_PROPERTY(int     matchCount  READ matchCount  NOTIFY matchesChanged)
    Q_PROPERTY(QString emptyText   READ emptyText   CONSTANT)

public:
    explicit TagController(Backend *backend, TagModel *model, QObject *parent = nullptr);

    bool    supported() const { return m_backend != nullptr; }
    int     count() const { return m_tags.size(); }
    bool    busy() const { return m_busy; }
    QString filterTagId() const { return m_filterTagId; }
    int     matchCount() const { return m_matches.size(); }
    QString emptyText() const;

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void create(const QString &name, const QString &color);
    Q_INVOKABLE void remove(const QString &tagId);
    // 全量设置某文件的标签（勾选完一次性提交）
    Q_INVOKABLE void assign(const QString &fileId, const QStringList &tagIds);
    // 拉取某文件当前的标签 → 异步回 fileTagsLoaded(fileId, tagIds)
    Q_INVOKABLE void loadFileTags(const QString &fileId);
    // 按标签筛选文件（tagId 为空 = 取消筛选）
    Q_INVOKABLE void applyFilter(const QString &tagId);
    // 筛选命中的文件（供 QML ListView 直接消费）
    Q_INVOKABLE QVariantList matchedFiles() const;
    // 关键词检索（TagsPage 顶部搜索框）
    Q_INVOKABLE void search(const QString &keyword);

signals:
    void tagsChanged();
    void busyChanged();
    void filterChanged();
    void matchesChanged();
    void errorOccurred(const QString &message);
    void statusMessage(const QString &message, bool ok);
    void logMessage(const QString &level, const QString &text);
    void fileTagsLoaded(const QString &fileId, const QStringList &tagIds);
    // 某个文件的标签被改过了。**专用**信号，不复用 tagsChanged：
    // tagsChanged 在"标签列表刷新"时也会发（那是另一件事），拿它驱动文件列表刷新会多打无谓的请求。
    void fileTagsAssigned(const QString &fileId);
    void tagCreated(const QString &name);

private:
    void        setBusy(bool on);
    static QString friendlyError(const QString &raw);

    Backend       *m_backend = nullptr;
    TagModel      *m_model   = nullptr;
    QVector<Tag>       m_tags;
    QVector<FileItem>  m_matches;
    QString m_filterTagId;
    bool    m_busy = false;
    int     m_seq  = 0; // 序号：丢弃乱序回包
};

} // namespace cv
