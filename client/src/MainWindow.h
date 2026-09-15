#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QSet>
#include <QString>
#include <QUrl>

class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSplitter;
class QTableWidget;
class QTableWidgetItem;

// 单个分块在文件中的位置：从 offset 起读 length 字节
struct ChunkPlan
{
    qint64 offset = 0;   // 分块起始偏移
    qint64 length = 0;   // 分块字节数（最后一块可能不足 chunk_size）
};

// 云匣 CloudVault 的 Qt 6 测试客户端主窗口。
// 用一组按钮直接驱动服务端接口：健康检查 / 整文件上传 / 列表 / 整文件下载 /
// 分块上传（断点续传）/ 分块下载（断点续传），并把每次请求的方法、URL、状态码、
// 耗时与响应原文打到日志区，便于肉眼验收。
// 文件列表展示 名称 / 大小 / 时间，右侧侧边栏会在选中行变化时预览该文件内容。
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void onHealthCheck();   // 【健康检查】 GET /healthz
    void onUpload();        // 【选择文件并上传】 POST /api/v1/files（整文件，保留）
    void onListFiles();     // 【列出文件】     GET /api/v1/files
    void onDownload();      // 【下载选中文件】 GET /api/v1/files/:id/content（整文件，保留）
    void onClearLog();      // 【清空日志】
    void onChunkedUpload(); // 【分块上传（断点续传）】 init -> PUT chunk* -> complete
    void onCancelUpload();  // 【取消上传】 DELETE /api/v1/uploads/:id
    void onResumableDownload();  // 【分块下载（断点续传）】 GET /content 带 Range
    void onSelectionChanged();   // 列表选中行变化 -> 拉取并渲染预览
    void onForcePreview();       // 【仍要预览】超过体积上限时由用户显式确认
    void onReplyFinished(QNetworkReply *reply);

private:
    // ---- 界面搭建 ----
    QWidget *createTopBar();       // 顶部：服务器地址 + 按钮 + 进度条
    QWidget *createCenter();       // 中部：文件列表（左）+ 预览侧边栏（右）
    QWidget *createPreviewPanel(); // 右侧：预览信息行 + 只读文本区 + 【仍要预览】

    // ---- 网络辅助 ----
    // 把服务器基地址与接口路径拼成完整 URL，自动去掉基地址结尾的 '/'
    QUrl buildUrl(const QString &path) const;
    // 发出请求并登记起始时间；内部会维护"忙碌中"状态，防止重复点击。
    // 支持 GET / POST / PUT / DELETE；verb 记到 cvVerb 属性上供日志使用。
    QNetworkReply *sendRequest(const QNetworkRequest &request, const QByteArray &verb,
                               const QByteArray &body = QByteArray());
    // 请求完成后的统一收口：写日志 + 按 cvStep（分块会话）或 URL 分派业务处理
    void handleReply(QNetworkReply *reply, const QByteArray &raw);
    void handleUploadReply(const QByteArray &raw);
    void handleListReply(const QByteArray &raw);
    void handleDownloadReply(const QByteArray &raw);   // 把整文件下载内容写到用户选择的路径

    // ---- 分块上传（断点续传）----
    // 流式算整文件 SHA-256，同时按 chunk_size 切出分块表（不把整文件读进内存）
    bool prepareChunkPlan(const QString &path, QString *err);
    bool loadManifest(const QString &path);   // 读本地 manifest，校验 size/mtime/hash
    void saveManifest();                      // 把当前会话落盘（每收到一个分块 ACK 都调用）
    void clearManifest();                     // 上传完成后删除 manifest
    void sendGetSession();   // GET  /api/v1/uploads/:id（续传前确认 upload_id 是否有效）
    void sendInit();         // POST /api/v1/uploads/init
    void beginUploading(const QJsonArray &serverUploaded);  // 合并已传集合并启动并发上传
    void pumpChunks();       // 按并发上限补齐在途分块；全部完成则 complete
    void launchChunk(int seq);     // PUT 单块（带 X-Chunk-SHA256，指数退避重试）
    void sendComplete();     // POST /api/v1/uploads/:id/complete
    // 分块会话内所有响应的统一收口，按 step 分派；
    // dlTotal / dlStart 取自 Content-Range（仅下载用，dlStart=-1 表示 200 整文件响应）
    void handleChunkedReply(const QString &step, int seq, int status, bool networkError,
                            const QString &errorString, const QByteArray &raw,
                            qint64 dlTotal = 0, qint64 dlStart = -1);
    void handleGetSessionReply(int status, const QByteArray &raw);
    void handleInitReply(int status, const QByteArray &raw);
    void handleChunkReply(int status, const QString &errorString, const QByteArray &raw,
                          int seq);
    void handleCompleteReply(int status, const QString &errorString, const QByteArray &raw);
    bool readChunk(int seq, QByteArray *out, QString *err) const;  // seek + read 单块
    int uploadedCount() const;                                     // 已确认分块数（0..total）
    void updateProgress();       // 刷新进度条与进度文案
    void updateCancelButton();   // 【取消上传】只在上传会话进行中可用
    // 会话收尾：ok=false 时保留 upload_id 与已传分块表，供下次续传
    void finishChunkSession(bool ok);

    // ---- 分块下载（断点续传）----
    void sendDownloadRange(qint64 offset);  // GET /content，带 Range: bytes=offset-
    void handleDownloadChunkReply(int status, bool networkError, const QString &errorString,
                                  const QByteArray &raw, qint64 dlTotal, qint64 dlStart);
    void finalizeDownload();     // 校验大小，.part -> 正式文件
    void finishDownload(bool ok);

    // ---- 预览 ----
    void cancelPreview();                                   // 中止并丢弃在途的预览请求
    void startPreview(const QString &id);                   // 发起预览请求
    void renderPreview(const QByteArray &data);             // 文本 / 十六进制渲染
    void setPreviewHeader(const QString &name, qint64 size, qint64 createdAt);
    void resetPreview();                                    // 未选中任何行时的空状态
    // 预览响应收口：区分 成功 / HTTP 非 200 / 网络错误 / 已取消
    void handlePreviewReply(int status, const QString &errorString, bool networkError,
                            const QByteArray &raw);

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
    QPushButton *m_clearLogBtn = nullptr;       // 【清空日志】
    QPushButton *m_chunkUploadBtn = nullptr;   // 【分块上传（断点续传）】
    QPushButton *m_cancelUploadBtn = nullptr;  // 【取消上传】
    QPushButton *m_dlResumeBtn = nullptr;       // 【分块下载（断点续传）】
    QProgressBar *m_progressBar = nullptr;     // 分块上传进度：已传块数 / 总块数
    QLabel *m_progressLabel = nullptr;         // 进度文案（含 upload_id / 续传命中）
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

    // 整文件下载是"先把用户选的保存路径记下来，响应回来再落盘"
    bool m_downloading = false;
    QString m_downloadId;
    QString m_downloadPath;

    // ---- 分块上传会话状态 ----
    QString m_chunkPath;          // 本地文件绝对路径
    QString m_chunkName;          // 文件名
    qint64 m_chunkFileSize = 0;   // 文件总字节数
    qint64 m_chunkMtime = 0;      // 文件修改时间（msec，用于 manifest 校验）
    QString m_chunkFileHash;      // 整文件 SHA-256（流式计算）
    qlonglong m_uploadId = 0;     // 服务端会话 id（续传依据）
    int m_totalChunks = 0;        // 分块总数（取自服务端 init 响应）
    QList<ChunkPlan> m_chunks;    // 每个分块的偏移/长度表
    QSet<int> m_doneSeq;          // 已确认分块序号（本地 manifest ∪ 服务端返回）
    QSet<int> m_inflightSeq;      // 在途分块序号（并发上限内）
    QHash<int, int> m_chunkRetries;  // 各分块已重试次数
    bool m_chunkActive = false;   // 是否有上传会话在进行（【取消上传】可用）
    bool m_cancelRequested = false;  // 用户点了取消
    bool m_resumeHit = false;        // 本次是否命中本地 manifest 续传
    bool m_completeSent = false;     // 防止 complete 被重复发送
    QString m_manifestDir;        // manifest 目录（AppDataLocation/cloudvault）

    // ---- 分块下载会话状态 ----
    bool m_dlActive = false;      // 是否有下载会话在进行
    QString m_dlId;               // 文件 id
    QString m_dlFinalPath;        // 最终落盘路径
    QString m_dlPartPath;         // 临时文件 <final>.part
    qint64 m_dlTotal = 0;         // 文件总字节数（来自列表 / Content-Range）
    qint64 m_dlOffset = 0;        // 已下载字节数（= .part 当前大小）
};
