/****************************************************************************
 * controllers/FileController.h —— 文件列表 / 文件操作控制器
 *
 * 契约：docs/整合实现契约.md §3（冻结 API）
 *   · 属性：currentDir、loading、emptyText
 *   · 方法：refresh()、enterDir(dir)、createFile(name)、rename(id,name)、
 *           remove(id)、createFolder(path)、uploadHere()、downloadSelected()
 *   列表数据走 data/FileListModel（契约 §4 角色名：name/size/time/status/id/dir）。
 *
 * 目录语义（对齐 HttpBackend 的真实契约）：
 *   · currentDir 是「相对路径」（'' = 根目录，如 "照片/2026"），不是 id；
 *   · 进入子文件夹：newDir = 父行的 dir 字段 + '/' + 该行 name（根目录时无前导 '/'）。
 *
 * 追加 API（不动冻结项，为无参方法提供必要输入 / 供 QML 接线）：
 *   · selectedIds 属性 + setSelectedIds(list) —— QML 列表选中项回填后，
 *     downloadSelected() 据此派发 downloadRequested(ids)；
 *   · uploadHere() 派发 uploadRequested(dir)，由 QML 打开文件选择框后
 *     调用 TransferController.upload(paths, dir)。
 ****************************************************************************/
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include "core/Types.h" // FileItem

namespace cv {

class Backend;
class FileListModel;

class FileController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString     currentDir  READ currentDir  NOTIFY currentDirChanged)
    Q_PROPERTY(bool        loading     READ loading     NOTIFY loadingChanged)
    Q_PROPERTY(QString     emptyText   READ emptyText   NOTIFY emptyTextChanged)
    Q_PROPERTY(QStringList selectedIds READ selectedIds WRITE setSelectedIds NOTIFY selectedIdsChanged)
    Q_PROPERTY(bool        supported   READ supported   CONSTANT)

public:
    explicit FileController(Backend *backend, FileListModel *model, QObject *parent = nullptr);

    QString     currentDir() const { return m_currentDir; }
    bool        loading() const { return m_loading; }
    QString     emptyText() const { return m_emptyText; }
    QStringList selectedIds() const { return m_selectedIds; }
    void        setSelectedIds(const QStringList &ids);
    bool        supported() const { return m_backend != nullptr; }

    // ---- 契约 §3 冻结方法 ----
    Q_INVOKABLE void refresh();                          // 重新列出 currentDir
    // 追加：低频轮询入口（自动刷新用）。异步列目录，**内容指纹未变则不动界面、不发任何信号**
    // （连 refreshFinished 也不发），避免"无变化也每轮刷 UI / 弹一句提示"。
    // 与 refresh() 共用同一套异步回包处理与 m_refreshSeq 序号（两者混发时旧回包一律丢弃）。
    Q_INVOKABLE void refreshIfChanged();
    Q_INVOKABLE void enterDir(const QString &dir);       // 进入相对目录（'' = 根）
    Q_INVOKABLE void createFile(const QString &name);    // 新建空文件
    Q_INVOKABLE void rename(const QString &id, const QString &name);
    Q_INVOKABLE void remove(const QString &id);          // 真实删除（契约 §8.1）
    Q_INVOKABLE void createFolder(const QString &path);  // 相对路径，自动补齐中间目录
    Q_INVOKABLE void uploadHere();                       // 请求 QML 选择文件并上传到 currentDir
    Q_INVOKABLE void downloadSelected();                 // 下载 selectedIds

    // ---- 追加：导航辅助 ----
    Q_INVOKABLE void goUp();                             // 返回上一级（根目录时无操作）

signals:
    void currentDirChanged();
    void loadingChanged();
    void emptyTextChanged();
    void selectedIdsChanged();

    void errorOccurred(const QString &message);      // 供 QML 弹确认框 / 提示
    void statusMessage(const QString &message, bool ok);
    // refresh() 结束时**恰好发一次**（成功 true / 失败 false），供界面驱动
    // 「自动刷新失败即暂停 / 成功即恢复」。**不要**用它弹提示文案（Main.qml 的
    // toast 接线对任何非空 message 都会弹，会退化成每轮弹窗）。
    // refreshIfChanged()（低频轮询）：仅「失败」或「内容指纹变化」时才发；未变化不发任何信号。
    void refreshFinished(bool ok);
    void uploadRequested(const QString &dir);          // QML 打开文件选择框后调 Transfer.upload
    void downloadRequested(const QStringList &fileIds); // QML 或上层转交 Transfer.download
    void logMessage(const QString &level, const QString &text);

private:
    void setLoading(bool on);
    void setEmptyText(const QString &text);
    void setCurrentDir(const QString &dir);
    void setItems(const QVector<FileItem> &items);
    static QString normalizeDir(const QString &dir); // 去首尾 '/'，'/' -> ''

    // refresh() 与 refreshIfChanged() **共用**的异步入口（pollOnly=true 即低频轮询）。
    // 二者共用 m_refreshSeq 序号：混发时旧回包一律丢弃（不 emit、不改状态）。
    void beginLoad(bool pollOnly);
    // **共用**的回包处理（唯一一份，杜绝双路径漂移）：
    //   · 失败  -> 与 refresh() 失败路径完全一致（恰好一次 refreshFinished(false)）
    //   · 成功 + 非轮询 -> 应用列表 + 恰好一次 refreshFinished(true)，并更新指纹缓存
    //   · 成功 + 轮询 且 指纹未变 -> 不 setItems / 不 setEmptyText / 不发任何信号（含 refreshFinished）
    //   · 成功 + 轮询 且 指纹已变 -> 应用列表 + 恰好一次 refreshFinished(true)，并更新指纹缓存
    void handleFolderReply(const QString &dir, const Result<QVector<FileItem>> &reply, bool pollOnly);
    // 内容指纹：按「id|name|size|mtime」稳定排序后整体 SHA-256（判定轮询是否"变了"）
    static QString fingerprintOf(const QVector<FileItem> &items);

    Backend       *m_backend = nullptr;
    FileListModel *m_model   = nullptr;

    QString       m_currentDir;  // '' = 根目录
    bool          m_loading = false;
    QString       m_emptyText;
    QStringList   m_selectedIds;
    QVector<FileItem> m_items;   // 当前目录项（供 id 校验 / 选中修剪）
    int           m_refreshSeq = 0; // 刷新序号：丢弃乱序（旧目录）回包，防快速切换目录串台
    QString       m_lastFingerprint; // 最近一次成功应用列表的内容指纹（轮询据此判断"是否变了"）
};

} // namespace cv
