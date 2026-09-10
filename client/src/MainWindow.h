#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QMainWindow>
#include <QString>
#include <QUrl>

class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;
class QPlainTextEdit;
class QPushButton;
class QSplitter;
class QTableWidget;
class QTableWidgetItem;

// 云匣 CloudVault 的 Qt 6 测试客户端主窗口。
// 用一组按钮直接驱动服务端接口：健康检查 / 上传 / 列表 / 下载，
// 并把每次请求的方法、URL、状态码、耗时与响应原文打到日志区，便于肉眼验收。
// 文件列表展示 名称 / 大小 / 时间，右侧侧边栏会在选中行变化时预览该文件内容。
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void onHealthCheck();   // 【健康检查】 GET /healthz
    void onUpload();        // 【选择文件并上传】 POST /api/v1/files
    void onListFiles();     // 【列出文件】     GET /api/v1/files
    void onDownload();      // 【下载选中文件】 GET /api/v1/files/:id/content
    void onClearLog();      // 【清空日志】
    void onSelectionChanged();   // 列表选中行变化 -> 拉取并渲染预览
    void onForcePreview();       // 【仍要预览】超过体积上限时由用户显式确认
    void onReplyFinished(QNetworkReply *reply);

private:
    // ---- 界面搭建 ----
    QWidget *createTopBar();       // 顶部：服务器地址 + 五个按钮
    QWidget *createCenter();       // 中部：文件列表（左）+ 预览侧边栏（右）
    QWidget *createPreviewPanel(); // 右侧：预览信息行 + 只读文本区 + 【仍要预览】

    // ---- 网络辅助 ----
    // 把服务器基地址与接口路径拼成完整 URL，自动去掉基地址结尾的 '/'
    QUrl buildUrl(const QString &path) const;
    // 发出请求并登记起始时间；内部会维护"忙碌中"状态，防止重复点击
    QNetworkReply *sendRequest(const QNetworkRequest &request, const QByteArray &verb,
                               const QByteArray &body = QByteArray());
    // 请求完成后的统一收口：写日志 + 按 URL 分派业务处理
    void handleReply(QNetworkReply *reply, const QByteArray &raw);
    void handleUploadReply(const QByteArray &raw);
    void handleListReply(const QByteArray &raw);
    void handleDownloadReply(const QByteArray &raw);   // 把下载内容写到用户选择的路径
    // 预览响应收口：区分 成功 / HTTP 非 200 / 网络错误 / 已取消
    void handlePreviewReply(int status, const QString &errorString, bool networkError,
                            const QByteArray &raw);

    // ---- 预览 ----
    void cancelPreview();                                   // 中止并丢弃在途的预览请求
    void startPreview(const QString &id);                   // 发起预览请求
    void renderPreview(const QByteArray &data);             // 文本 / 十六进制渲染
    void setPreviewHeader(const QString &name, qint64 size, qint64 createdAt);
    void resetPreview();                                    // 未选中任何行时的空状态

    // ---- 状态与展示 ----
    void setBusy(bool busy);                 // 忙碌时禁用网络按钮
    void appendLog(const QString &method, const QUrl &url, int status, qint64 elapsedMs,
                   const QString &body);
    // doSelect=false：插完行不改选中，避免刷新列表 / 上传完成时顺带拉一次预览
    void addRow(const QString &id, const QString &name, qint64 size, const QString &hash,
                const QString &instantText, qint64 createdAt, bool doSelect = false);
    QString currentFileId() const;           // 列表当前选中行的 id
    QString currentFileName() const;         // 列表当前选中行的文件名
    qint64 currentFileSize() const;          // 列表当前选中行的字节数
    qint64 currentFileCreatedAt() const;     // 列表当前选中行的创建时间（created_at 毫秒）

    static QString formatSize(qint64 bytes); // 字节数转人类可读
    static QString formatBody(const QByteArray &raw);  // 响应原文（过长截断）
    // 时间列格式化：入口把服务端的毫秒归一化成秒，再转本地时间字符串
    static QString formatTime(qint64 epoch);
    static bool looksBinary(const QByteArray &data);   // 含 '\0' 或非法 UTF-8 -> 二进制
    static QString toHexDump(const QByteArray &data);  // 16 字节一行的十六进制转储

    // ---- 成员 ----
    QLineEdit *m_serverEdit = nullptr;
    QPushButton *m_healthBtn = nullptr;
    QPushButton *m_uploadBtn = nullptr;
    QPushButton *m_listBtn = nullptr;
    QPushButton *m_downloadBtn = nullptr;
    QPushButton *m_clearLogBtn = nullptr;
    QTableWidget *m_table = nullptr;
    QPlainTextEdit *m_log = nullptr;

    QSplitter *m_splitter = nullptr;         // 左：文件列表  右：预览侧边栏
    QLabel *m_previewName = nullptr;         // 预览区顶部：文件名
    QLabel *m_previewMeta = nullptr;         // 预览区顶部：大小 / 时间
    QPlainTextEdit *m_previewText = nullptr; // 预览区主体（只读、等宽）
    QPushButton *m_previewForceBtn = nullptr;// 【仍要预览】，超过体积上限时才出现

    QNetworkAccessManager *m_nam = nullptr;
    QHash<QNetworkReply *, QElapsedTimer> m_timers;   // 每个请求各自的计时器
    QHash<QString, bool> m_instantById;               // 文件 id -> 是否秒传
    int m_pending = 0;                                // 在途请求数

    QNetworkReply *m_previewReply = nullptr; // 在途的预览请求（切换选中行时先中止它）
    QString m_previewId;                     // 当前正在预览 / 待预览的文件 id

    // 下载是"先把用户选的保存路径记下来，响应回来再落盘"
    bool m_downloading = false;
    QString m_downloadId;
    QString m_downloadPath;
};
