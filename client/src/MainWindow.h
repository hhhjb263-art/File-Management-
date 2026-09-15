#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QMap>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>

class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QNetworkReply;
class QThreadPool;
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

// 批量下载队列项（id + 名称 + 大小；targetPath 非空表示单文件指定了保存路径）
struct DlItem
{
    QString id;
    QString name;
    qint64 size = 0;
    QString targetPath;
};

// 后台线程池任务结果（阻塞式文件 IO / 哈希全部移出 UI 线程）
struct ChunkScanResult
{
    bool ok = false;
    QString err;
    QString path;
    QString name;
    QString hash;
    qint64 size = 0;
    qint64 mtime = 0;
    QList<ChunkPlan> chunks;
};

struct ChunkReadResult
{
    QString hash;      // 分块 SHA-256（线程池内算好，避免占用 UI 线程）
    bool ok = false;
    QString err;
    int seq = 0;
    QByteArray data;
};

// 文件列表行数据（客户端侧数据模型：排序 / 渲染都基于它）
struct RowData
{
    QString id;
    QString name;
    QString hash;
    QString instantText;   // 是否秒传（本地记录）
    QString dir;           // 所属目录（'' = 根目录）
    qint64 size = 0;
    qint64 createdAt = 0;
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

    // ---- 可被其它模块复用的静态工具（文件树对话框等）----
    // 客户端侧路径预检（与服务端 sanitizeRelPath 同规则；不做最终裁决）
    static bool validRelPathInput(const QString &in, QString *why);
    static QString formatSize(qint64 bytes);   // 字节数转人类可读
    static QString formatTime(qint64 epoch);   // 时间列格式化：入口把服务端的毫秒归一化成秒，再转本地时间字符串
    static QString formatBody(const QByteArray &raw);  // 响应原文（过长截断）

private slots:
    void onHealthCheck();   // 【健康检查】 GET /healthz
    void onUpload();        // 【选择文件并上传】 POST /api/v1/files（整文件，保留）
    void onListFiles();     // 【列出文件】     GET /api/v1/files
    void onDownload();      // 【下载选中文件】 GET /api/v1/files/:id/content（整文件，保留）
    void onClearLog();      // 【清空日志】
    void onChunkedUpload(); // 【分块上传（断点续传）】 init -> PUT chunk* -> complete
    void onCancelUpload();  // 【取消上传】 DELETE /api/v1/uploads/:id
    void onResumableDownload();  // 【分块下载（断点续传）】 GET /content 带 Range
    // 文件列表右键菜单动作
    void onTableContextMenu(const QPoint &pos);
    void onCreateFile();     // 新建空文件（POST /api/v1/files/new）
    void onRenameFile();     // 重命名选中文件（POST /api/v1/files/:id/rename）
    void onDeleteFile();     // 删除选中文件（DELETE /api/v1/files/:id）
    void onUploadToDir();    // 上传到选中行所在目录
    void onOpenFolder();     // 打开本地保存文件夹（定位已下载文件）
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
    void handleUploadReply(int status, const QByteArray &raw);
    void handleListReply(const QByteArray &raw);

    // ---- 分块上传（断点续传）----
    bool loadManifest(const QString &path);   // 读本地 manifest，校验 size/mtime/hash
    void saveManifest();                      // 把当前会话落盘（每收到一个分块 ACK 都调用）
    void clearManifest();                     // 上传完成后删除 manifest
    void sendGetSession();   // GET  /api/v1/uploads/:id（续传前确认 upload_id 是否有效）
    void sendInit();         // POST /api/v1/uploads/init
    void beginUploading(const QJsonArray &serverUploaded);  // 合并已传集合并启动并发上传
    void pumpChunks();       // 按并发上限补齐在途分块；全部完成则 complete
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

    // ---- 批量上传 / 下载（多选）----
    void startNextUpload();      // 顺序处理队列中的下一个本地文件（小文件整传、大文件分块）
    void startNextDownload();    // 顺序下载队列中的下一个服务器文件（Range 分段）
    // 下载滑动窗口（并发拉取多段，按序落盘；.part 追加语义不变，断点续传仍有效）
    void pumpDownload();          // 补发在途请求，保持窗口满
    void drainDownloadReady();    // 把按序就绪的段移入写盘队列
    void maybeStartWrite();       // 写盘队列空闲则启动下一段写
    void checkDownloadDone();     // 全部段写完 → finalize
    void showStatus(const QString &text, bool ok);   // 顶部 ✓/✗ 简化结果标识
    void refreshStorage();       // GET /api/v1/storage：刷新"服务器剩余空间"显示
    void applyStorageInfo(const QByteArray &raw);   // 解析并更新空间标签
    // 建立一次分块上传会话（流式：5MiB/块，绝不整文件入内存）；失败时 err 非空
    bool beginChunkedUploadFor(const QString &path, const QString &dir, QString *err);
    // 发送单个小文件（整文件 POST）；overwrite=true 时带 X-CV-Overwrite 覆盖同名
    void sendWholeFile(const QString &path, bool overwrite);
    // 同名冲突：返回 true = 用户选择覆盖，false = 跳过
    bool askOverwrite(const QString &dir, const QString &name);

    // ---- 线程池：阻塞式文件 IO / 哈希移出 UI 线程 ----
    void onChunkScanDone(const ChunkScanResult &r);      // 扫描（含 SHA-256）完成
    void afterChunkPlanReady(const QString &dir);        // 拿到分块表后继续建立会话
    void launchChunkAsync(int seq);                      // 后台读分块 → 回主线程发送
    void onChunkReadDone(const ChunkReadResult &r);
    void appendDownloadSegmentAsync(const QByteArray &data, qint64 offset);  // 后台追加落盘
    void onDownloadSegmentWritten(bool ok, const QString &err, qint64 bytes);

    // ---- 文件列表数据模型与排序 ----
    void renderRows();                  // 按 m_rows 重建表格
    void applySort(int key, bool asc);  // key: 0=名称 1=大小 2=时间
    void handleNewFileReply(int status, const QByteArray &raw);
    void handleRenameReply(int status, const QByteArray &raw);
    void handleDeleteReply(int status, const QByteArray &raw);

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
                const QString &instantText, qint64 createdAt, bool doSelect = false,
                const QString &dir = QString());
    QString currentFileId() const;           // 列表当前选中行的 id
    QString currentFileName() const;         // 列表当前选中行的文件名
    qint64 currentFileSize() const;          // 列表当前选中行的字节数
    qint64 currentFileCreatedAt() const;     // 列表当前选中行的创建时间（created_at 毫秒）

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
    QLabel *m_statusLabel = nullptr;            // 顶部结果标识（✓ 成功 / ✗ 失败）
    QLabel *m_spaceLabel = nullptr;             // 顶部：服务器剩余空间显示
    QTimer *m_spaceTimer = nullptr;             // 空间显示定时刷新
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
    QThreadPool *m_ioPool = nullptr;   // 文件 IO / 哈希线程池（默认 3 线程）
    // ⚠️ 线程安全规则（无锁模型的前提）：池内 lambda 只允许捕获「值拷贝」，
    //    禁止捕获 this / 读写任何 m_ 成员；结果必须经 QMetaObject::invokeMethod(
    //    self, …, Qt::QueuedConnection) 回主线程后再改成员。违反即引入数据竞争。
    bool m_scanning = false;           // 正在后台扫描文件（防重入）
    QString m_pendingChunkDir;         // 异步扫描期间暂存目标目录
    QHash<QNetworkReply *, QElapsedTimer> m_timers;   // 每个请求各自的计时器
    QHash<QString, bool> m_instantById;               // 文件 id -> 是否秒传
    int m_pending = 0;                                // 在途请求数

    QNetworkReply *m_previewReply = nullptr; // 在途的预览请求（切换选中行时先中止它）
    QString m_previewId;                     // 当前正在预览 / 待预览的文件 id

    // 下载统一走 Range 分段（见分块下载会话状态）；不再有"整文件读进内存再落盘"的路径

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
    QString m_chunkDir;           // 本次分块上传的目标目录（'' = 根目录）
    QString m_lastDir;            // 上一次使用的目标目录（输入框默认值）

    // ---- 分块下载会话状态 ----
    bool m_dlActive = false;      // 是否有下载会话在进行
    QString m_dlId;               // 文件 id
    QString m_dlFinalPath;        // 最终落盘路径
    QString m_dlPartPath;         // 临时文件 <final>.part
    qint64 m_dlTotal = 0;         // 文件总字节数（来自列表 / Content-Range）
    qint64 m_dlOffset = 0;        // 已下载字节数（= .part 当前大小）

    // ---- 批量上传 / 下载队列（多选）----
    QStringList m_upQueue;        // 待上传的本地文件绝对路径
    QString m_upDir;              // 本次批量上传的目标目录（'' = 根目录）
    QString m_upCurrentPath;      // 当前正在上传的本地文件（同名冲突重发用）
    bool m_upSpaceWarned = false;   // 批量上传中空间不足只告警一次
    bool m_chunkOverwrite = false;  // 分块上传是否已确认覆盖同名
    int m_upOk = 0;
    int m_upFail = 0;
    bool m_upBatch = false;       // 是否处于批量上传中（响应回来后推进队列）
    QList<DlItem> m_dlQueue;      // 待下载（含大小，用于判定走不分段）
    QString m_dlDir;              // 批量下载的保存目录
    int m_dlOk = 0;
    int m_dlFail = 0;
    bool m_dlBatch = false;       // 是否处于批量下载中
    // ---- 下载滑动窗口状态 ----
    QMap<qint64, QByteArray> m_dlReady;      // 已到达、等待按序写盘的段（起点 → 数据）
    QList<QPair<qint64, QByteArray>> m_dlWriteQueue;  // 按序写盘队列（保证 .part 顺序）
    int m_dlInflight = 0;         // 在途请求数
    qint64 m_dlNextReq = 0;       // 下一段请求起点
    bool m_dlWriting = false;     // 是否有写盘任务在途（写盘串行化，防乱序）
    // ---- 本地保存位置记录（右键"打开文件夹"用）----
    QString m_lastSaveDir;        // 最近一次下载目录
    QHash<QString, QString> m_localPathById;   // file_id → 本地已下载路径

    // ---- 文件列表数据模型 ----
    QList<RowData> m_rows;        // 表格数据源（排序后重建渲染）
    QString m_ctxDir;             // 右键菜单所在行的目录（'' = 根目录）
    int m_sortKey = 2;            // 当前排序键：0=名称 1=大小 2=时间
    bool m_sortAsc = false;       // 升/降序
};
