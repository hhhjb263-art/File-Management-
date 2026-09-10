#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QMainWindow>
#include <QString>
#include <QUrl>

class QLineEdit;
class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTableWidgetItem;

// 云匣 CloudVault 的 Qt 6 测试客户端主窗口。
// 用一组按钮直接驱动服务端接口：健康检查 / 上传 / 列表 / 下载，
// 并把每次请求的方法、URL、状态码、耗时与响应原文打到日志区，便于肉眼验收。
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
    void onReplyFinished(QNetworkReply *reply);

private:
    // ---- 界面搭建 ----
    QWidget *createTopBar();    // 顶部：服务器地址 + 五个按钮
    QWidget *createCenter();    // 中部：文件列表

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

    // ---- 状态与展示 ----
    void setBusy(bool busy);                 // 忙碌时禁用网络按钮
    void appendLog(const QString &method, const QUrl &url, int status, qint64 elapsedMs,
                   const QString &body);
    void addRow(const QString &id, const QString &name, qint64 size, const QString &hash,
                const QString &instantText);
    QString currentFileId() const;           // 列表当前选中行的 id
    QString currentFileName() const;         // 列表当前选中行的文件名

    static QString formatSize(qint64 bytes); // 字节数转人类可读
    static QString formatBody(const QByteArray &raw);  // 响应原文（过长截断）

    // ---- 成员 ----
    QLineEdit *m_serverEdit = nullptr;
    QPushButton *m_healthBtn = nullptr;
    QPushButton *m_uploadBtn = nullptr;
    QPushButton *m_listBtn = nullptr;
    QPushButton *m_downloadBtn = nullptr;
    QPushButton *m_clearLogBtn = nullptr;
    QTableWidget *m_table = nullptr;
    QPlainTextEdit *m_log = nullptr;

    QNetworkAccessManager *m_nam = nullptr;
    QHash<QNetworkReply *, QElapsedTimer> m_timers;   // 每个请求各自的计时器
    QHash<QString, bool> m_instantById;               // 文件 id -> 是否秒传
    int m_pending = 0;                                // 在途请求数

    // 下载是"先把用户选的保存路径记下来，响应回来再落盘"
    bool m_downloading = false;
    QString m_downloadId;
    QString m_downloadPath;
};
