#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QSet>
#include <QUrl>

class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class QNetworkAccessManager;

// 远端文件树选择对话框（模态）。打开即并发拉取 GET /api/v1/dirs 与 GET /api/v1/files，
// 在客户端合成目录树供用户选择"文件"或"目录"。对话框独立持有 QNetworkAccessManager，
// 不与 MainWindow 的日志区交互；新建文件夹也由对话框自身用同源 POST /api/v1/dirs 完成。
//
// 数据契约（冻结，已有接口，本次未新增）：
//   GET /api/v1/dirs  -> { total, items:["docs","docs/backup", ...] }  // 扁平规范化相对路径，不含根
//   GET /api/v1/files -> { total, items:[{id,name,dir,size,hash,chunks,created_at}] }  // dir="" 为根
class FileTreeDialog : public QDialog
{
    Q_OBJECT

public:
    enum class Mode { SelectFile, SelectDir };

    // dirsUrl/filesUrl: GET /api/v1/dirs 与 GET /api/v1/files 的完整 URL
    // mode: 选择类型（文件 / 目录）
    // initialPath: 上次使用的相对路径（用于默认选中/展开，如 m_lastDir）
    // allowCreateDir: 是否显示【新建文件夹】按钮；为 true 时以【关闭】代替【确定】
    FileTreeDialog(const QUrl &dirsUrl, const QUrl &filesUrl, Mode mode,
                   const QString &initialPath, bool allowCreateDir,
                   QWidget *parent = nullptr, bool trustTls = true,
                   const QString &authToken = QString());
    ~FileTreeDialog();

    // exec() 返回 Accepted 时调用方才取值；Rejected 不取值（取消 / 关闭）
    QString selectedPath() const { return m_selectedPath; }   // 文件=dir/name；目录=dir；根=""
    bool isFile() const { return m_selectedIsFile; }
    Mode mode() const { return m_mode; }

protected:
    void showEvent(QShowEvent *event) override;

private slots:
    void onCurrentChanged();
    void onItemDoubleClicked(QTreeWidgetItem *item, int column);
    void onOk();
    void onClose();        // 浏览/新建模式：关闭对话框
    void onExpandAll();
    void onCollapseAll();
    void onRefresh();
    void onRetry();
    void onCreateFolder(); // 新建文件夹（allowCreateDir 模式）
    void handleDirsReply(int status, bool networkError, const QByteArray &raw);
    void handleFilesReply(int status, bool networkError, const QByteArray &raw);
    void handleCreateReply(int status, bool networkError, const QByteArray &raw,
                           const QString &newPath);

private:
    void loadData();
    void maybeBuildTree();
    void buildTree();
    QTreeWidgetItem *rootItem() const;
    QTreeWidgetItem *ensureDir(const QString &path);   // 逐级补齐目录节点，返回末端节点
    void addFile(const QString &dir, const QString &name, qint64 size, qint64 createdAt);
    void setStatus(const QString &text);
    void applySelectionEnabled();
    bool isDirItem(QTreeWidgetItem *item) const;
    QString itemPath(QTreeWidgetItem *item) const;
    void restoreExpanded();
    void highlightInitial();

    QUrl m_dirsUrl;
    QUrl m_filesUrl;
    Mode m_mode;
    QString m_initialPath;
    bool m_allowCreateDir;
    bool m_trustTls = true;       // 信任自签名证书（HTTPS 场景）
    QString m_authToken;          // 访问令牌（Bearer）；来自 MainWindow 的输入框，留空不带 Token

    QTreeWidget *m_tree = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_okBtn = nullptr;       // 【确定】（选择文件 / 目录 / 上传位置）
    QPushButton *m_createBtn = nullptr;   // 【新建文件夹】（allowCreateDir 模式才有）
    QPushButton *m_retryBtn = nullptr;

    QNetworkAccessManager *m_nam = nullptr;

    QJsonArray m_dirsItems;   // /api/v1/dirs 的 items（字符串列表）
    QJsonArray m_filesItems;  // /api/v1/files 的 items（对象列表）
    int m_pending = 0;        // 两个列表请求同时在途计数
    bool m_loadError = false;
    QString m_loadErrorMsg;

    bool m_firstBuild = true;             // 仅首次默认展开根 + 一级目录
    QSet<QString> m_expandedPaths;        // 当前已展开的目录 path；展开/折叠信号实时维护

    QString m_selectedPath;
    bool m_selectedIsFile = false;
};
