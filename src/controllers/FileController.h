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
#include <QVariantMap>
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
    // ---- 文件预览（加法式）：异步取内容区间，绝不阻塞主线程 ----
    Q_PROPERTY(QVariantMap previewMeta     READ previewMeta     NOTIFY previewChanged)
    Q_PROPERTY(QString     previewState    READ previewState    NOTIFY previewChanged)
    Q_PROPERTY(QString     previewText     READ previewText     NOTIFY previewChanged)
    Q_PROPERTY(QString     previewImageUrl READ previewImageUrl NOTIFY previewChanged)
    Q_PROPERTY(QString     previewError    READ previewError    NOTIFY previewChanged)

public:
    explicit FileController(Backend *backend, FileListModel *model, QObject *parent = nullptr);

    QString     currentDir() const { return m_currentDir; }
    bool        loading() const { return m_loading; }
    QString     emptyText() const { return m_emptyText; }
    QStringList selectedIds() const { return m_selectedIds; }
    void        setSelectedIds(const QStringList &ids);
    bool        supported() const { return m_backend != nullptr; }

    // ---- 预览属性（只读；见 requestPreview）----
    QVariantMap previewMeta() const { return m_previewMeta; }
    QString     previewState() const { return m_previewState; }
    QString     previewText() const { return m_previewText; }
    QString     previewImageUrl() const { return m_previewImageUrl; }
    QString     previewError() const { return m_previewError; }

    // 请求预览某文件（异步）。传空串等价 clearPreview()。
    Q_INVOKABLE void requestPreview(const QString &fileId);
    Q_INVOKABLE void clearPreview();

    // ---- 契约 §3 冻结方法 ----
    Q_INVOKABLE void refresh();                          // 重新列出 currentDir
    // 追加：低频轮询入口（自动刷新用）。异步列目录，**内容指纹未变则不动界面、不发任何信号**
    // （连 refreshFinished 也不发），避免"无变化也每轮刷 UI / 弹一句提示"。
    // 与 refresh() 共用同一套异步回包处理与 m_refreshSeq 序号（两者混发时旧回包一律丢弃）。
    Q_INVOKABLE void refreshIfChanged();
    Q_INVOKABLE void enterDir(const QString &dir);       // 进入相对目录（'' = 根）
    Q_INVOKABLE void createFile(const QString &name);    // 新建空文件
    Q_INVOKABLE void rename(const QString &id, const QString &name);
    // 目录树 CRUD：把文件移到另一个目录（targetParentId 与列表里的目录项 id 同一套）。
    Q_INVOKABLE void moveTo(const QString &id, const QString &targetParentId);
    // 目录树 CRUD：目录重命名（dirPath 是目录的相对路径，不是 id —— 目录在两个数据源里
    // 的 id 口径不同，而 path 是共同的、稳定的标识）。
    Q_INVOKABLE void renameFolder(const QString &dirPath, const QString &newName);
    // 病毒查杀（主页面右键菜单）。异步执行：服务端可能要等远端查询，不能卡界面。
    Q_INVOKABLE void scanFile(const QString &fileId);
    // 目录树 CRUD：列出所有可选目标目录（供"移动到…"的选择器）。
    // 返回 [{id, label, depth}]：id 用**列表里的目录 id**（两个数据源各自的口径，
    // 调用方不需要知道差别）；label 是去掉前导斜杠的显示路径；depth 用于缩进成树形。
    Q_INVOKABLE QVariantList allFolders();
    // 移入回收站（软删除）。**不是**永久删除 —— 内容与引用计数原样保留，可在回收站页恢复。
    // 永久删除（purge）只从回收站页发起：那是不可逆动作，入口必须唯一且带二次确认。
    Q_INVOKABLE void remove(const QString &id);
    Q_INVOKABLE void createFolder(const QString &path);  // 相对路径，自动补齐中间目录
    Q_INVOKABLE void uploadHere();                       // 请求 QML 选择文件并上传到 currentDir
    Q_INVOKABLE void downloadSelected();
    // 用**系统默认程序**打开：本地已有且大小吻合就直接开，否则先下载再开。
    // 可执行文件一律拦截（见 isBlockedExecutable），并给出原因。
    Q_INVOKABLE void openWithSystem(const QString &fileId);
    // 预览面板「编辑」模式保存：把文本按**覆盖上传**写回云端。
    // 服务端在覆盖前会自动 captureCurrent() 留一个历史版本 ⇒ 改错了能回滚。
    Q_INVOKABLE void savePreviewText(const QString &fileId, const QString &text);                 // 下载 selectedIds
    // 批量操作：对**当前选中的全部**生效（原来只有单目标 remove/moveTo，
    // 菜单里取 selectedIds[0] ⇒ 多选时只动第一个，这是用户报的那个现象）。
    Q_INVOKABLE void removeSelected();                   // 批量移入回收站
    Q_INVOKABLE void moveSelectedTo(const QString &targetParentId);  // 批量移动

    // ---- 追加：导航辅助 ----
    Q_INVOKABLE void goUp();                             // 返回上一级（根目录时无操作）

signals:
    // 查杀结束：ok=false 时 error 说明原因（未启用/网络/不支持）
    void scanFinished(const QString &fileId, const QString &verdictText,
                      const QString &detail, bool blocked, bool remoteFailed, bool ok,
                      const QString &error);
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
    // 预览属性变化（previewMeta / previewState / previewText / previewImageUrl / previewError 任一变化）
    void previewChanged();
    // 文件增 / 删 / 改名 / 新建文件夹**成功**后发出（供 Application 触发 Stats.refresh()；失败不发）
    void storageChanged();
    void uploadRequested(const QString &dir);          // QML 打开文件选择框后调 Transfer.upload
    void downloadRequested(const QStringList &fileIds); // QML 或上层转交 Transfer.download
    // 「打开」时本地还没有副本 → 上层先下载（openWhenDone=true），下完再打开
    void openNeedsDownload(const QString &fileId, const QString &name);
    void previewSaved(const QString &fileId);   // 保存成功 → 面板退出编辑态
    void logMessage(const QString &level, const QString &text);

private:
    // 不让客户端直接"运行"的危险后缀（.exe/.bat/.ps1…）。
    // 注意：拦截的是"打开运行"，不是"预览/下载"——纯文本预览仍然可用。
    static bool isBlockedExecutable(const QString &name);

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

    // ---- 预览内部 ----
    // 由文件名 / 是否目录推断预览类别："dir" | "text" | "image" | "other"
    static QString previewKindFor(const FileItem &item);
    // 异步取回内容后的统一处理（含乱序丢弃）；seq 过期则不发任何信号
    void handlePreviewReply(int seq, const QString &kind, const Result<QByteArray> &reply);
    void resetPreviewToIdle(); // 清空各预览字段并置 idle

    Backend       *m_backend = nullptr;
    FileListModel *m_model   = nullptr;

    QString       m_currentDir;  // '' = 根目录
    bool          m_loading = false;
    QString       m_emptyText;
    QStringList   m_selectedIds;
    QVector<FileItem> m_items;   // 当前目录项（供 id 校验 / 选中修剪）
    int           m_refreshSeq = 0; // 刷新序号：丢弃乱序（旧目录）回包，防快速切换目录串台
    QString       m_lastFingerprint; // 最近一次成功应用列表的内容指纹（轮询据此判断"是否变了"）

    // ---- 预览状态（主线程独占）----
    QVariantMap m_previewMeta;
    QString     m_previewState = QStringLiteral("idle"); // idle|loading|meta|text|image|unsupported|error
    QString     m_previewText;
    QString     m_previewImageUrl;
    QString     m_previewError;
    int         m_previewSeq = 0; // 预览序号：**独立于 m_refreshSeq**，丢弃乱序预览回包
};

} // namespace cv
