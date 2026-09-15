// 云匣 CloudVault 测试客户端 —— 主窗口实现
//
// 与服务端约定的接口（详见 server/src/app/main.cpp 与团队冻结契约）：
//   GET  /healthz                          健康检查
//   POST /api/v1/files                     整文件上传（body 原样），头 X-CV-Name / X-CV-Dir=百分号编码
//   GET  /api/v1/files                     列表（含 dir）
//   GET  /api/v1/files/:id                 元数据
//   GET  /api/v1/files/:id/content         下载；支持 Range: bytes=start- -> 206
//   GET  /api/v1/download?path=            按路径下载（仅限已记录文件；越界 403）
//   POST /api/v1/dirs                      创建目录 {path}（201 新建 / 200 幂等 / 400 非法 / 403 越界）
//
// 分块上传（断点续传，本文件新增）：
//   POST   /api/v1/uploads/init            {name,size,chunk_size,hash}
//                                        -> {upload_id,name,size,chunk_size,hash,uploaded:[seq],received_bytes}
//   GET    /api/v1/uploads/:id             续传前确认会话是否有效（失效则 404）
//   PUT    /api/v1/uploads/:id/chunk/:seq  body=分块字节，头 X-Chunk-SHA256
//                                        -> {seq,received_bytes}（幂等）
//   POST   /api/v1/uploads/:id/complete    -> {file_id,...}；缺块 409{missing}；哈希不符 422{invalid}
//   DELETE /api/v1/uploads/:id             -> 204，取消会话
//
// 说明：列表接口不返回 instant 字段，因此"是否秒传"一列取自本地记录的上传结果，
//       未经由本客户端上传过的文件显示为 "-"。

#include "MainWindow.h"
#include "FileTreeDialog.h"

#include <algorithm>

#include <QCryptographicHash>
#include <QPointer>
#include <QThreadPool>
#include <QDir>
#include <QJsonArray>
#include <QMenu>
#include <QProgressBar>
#include <QStandardPaths>
#include <QTimer>

#include <QByteArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace {

// 日志里单条响应最多展示的字节数，避免大文件把日志区刷爆
constexpr int kMaxBodyInLog = 4096;

// 二进制响应体在日志里只展示前 N 字节的十六进制摘要（避免整段乱码）
constexpr qsizetype kBodyBinaryPreviewBytes = 48;

// 服务端默认端口
constexpr int kDefaultPort = 8080;

// 分块大小：5 MiB（与服务端契约一致；整文件哈希也按此切分块表）
constexpr qint64 kChunkSize = 5 * 1024 * 1024;

// 大文件阈值：超过该大小的文件不再"整文件读进内存"，改走流式/分块路径
constexpr qint64 kStreamUploadThreshold = 8 * 1024 * 1024;   // 上传 > 8MiB → 分块上传（5MiB/块）
constexpr qint64 kDownloadChunkSize = 4 * 1024 * 1024;       // 下载每次 Range 只拉 4MiB

// 流式算哈希 / 读块时的缓冲大小（1 MiB），避免整文件入内存
constexpr qint64 kHashBufferSize = 1024 * 1024;

// 分块上传并发上限（2~4 之间取 3）
constexpr int kMaxConcurrent = 3;

// 单块失败后的最大重试次数（指数退避 0.5/1/2 秒）
constexpr int kMaxRetries = 3;

// 预览渲染上限：超过只渲染前 64 KiB
constexpr qsizetype kMaxPreviewRenderBytes = 64 * 1024;

// 预览体积保护：超过 16 MiB 不自动拉取，需用户点【仍要预览】
constexpr qint64 kMaxAutoPreviewBytes = 16 * 1024 * 1024;

// 二进制预览最多转储的字节数（64 行 × 16 字节）
constexpr qsizetype kMaxHexDumpBytes = 1024;

// 二进制判定只扫描前 8 KiB，避免大文件白做功
constexpr qsizetype kBinaryScanBytes = 8 * 1024;

QString defaultServerUrl()
{
    return QStringLiteral("http://172.20.32.231:%1").arg(kDefaultPort);
}

}  // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("云匣 CloudVault 测试客户端"));
    resize(900, 640);

    m_nam = new QNetworkAccessManager(this);
    connect(m_nam, &QNetworkAccessManager::finished, this, &MainWindow::onReplyFinished);

    // 文件 IO / 哈希线程池：扫描整文件算 SHA-256、逐块读、下载落盘都在这里跑，
    // 保证 UI 线程只做界面操作（默认 3 线程，避免与网络并发叠加打满磁盘）
    m_ioPool = new QThreadPool(this);
    m_ioPool->setMaxThreadCount(3);

    // manifest 目录：AppDataLocation/cloudvault（程序退出也不丢进度）
    m_manifestDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/cloudvault");
    QDir().mkpath(m_manifestDir);

    // 整体布局：顶部工具条 / 中部列表 / 底部日志
    QWidget *center = new QWidget(this);
    QVBoxLayout *mainLayout = new QVBoxLayout(center);
    mainLayout->addWidget(createTopBar());
    mainLayout->addWidget(createCenter(), 1);

    m_log = new QPlainTextEdit(center);
    m_log->setReadOnly(true);
    m_log->setPlaceholderText(QStringLiteral("每次请求的方法 + URL、HTTP 状态码、耗时与响应原文会显示在这里"));
    m_log->setMaximumBlockCount(2000);   // 限制行数，长时间测试也不会越来越卡
    mainLayout->addWidget(m_log, 1);

    setCentralWidget(center);

    // 服务器剩余空间：启动即查询一次，之后每 60 秒自动刷新
    m_spaceTimer = new QTimer(this);
    m_spaceTimer->setInterval(60 * 1000);
    connect(m_spaceTimer, &QTimer::timeout, this, &MainWindow::refreshStorage);
    m_spaceTimer->start();
    QTimer::singleShot(0, this, [this] { refreshStorage(); });
}

// ---------------------------------------------------------------------------
//  界面搭建
// ---------------------------------------------------------------------------
QWidget *MainWindow::createTopBar()
{
    QWidget *bar = new QWidget(this);
    QVBoxLayout *barLayout = new QVBoxLayout(bar);
    barLayout->setContentsMargins(0, 0, 0, 0);

    // 第一行：服务器地址 + 七个按钮
    QWidget *row1 = new QWidget(this);
    QLabel *label = new QLabel(QStringLiteral("服务器地址："), row1);
    m_serverEdit = new QLineEdit(defaultServerUrl(), row1);
    m_serverEdit->setClearButtonEnabled(true);
    m_serverEdit->setMinimumWidth(240);

    m_healthBtn = new QPushButton(QStringLiteral("健康检查"), row1);
    m_uploadBtn = new QPushButton(QStringLiteral("选择文件并上传（可多选）"), row1);
    m_listBtn = new QPushButton(QStringLiteral("列出文件"), row1);
    m_downloadBtn = new QPushButton(QStringLiteral("下载选中文件（可多选）"), row1);
    m_chunkUploadBtn = new QPushButton(QStringLiteral("分块上传（断点续传）"), row1);
    m_cancelUploadBtn = new QPushButton(QStringLiteral("取消上传"), row1);
    m_dlResumeBtn = new QPushButton(QStringLiteral("分块下载（断点续传）"), row1);
    m_clearLogBtn = new QPushButton(QStringLiteral("清空日志"), row1);

    connect(m_healthBtn, &QPushButton::clicked, this, &MainWindow::onHealthCheck);
    connect(m_uploadBtn, &QPushButton::clicked, this, &MainWindow::onUpload);
    connect(m_listBtn, &QPushButton::clicked, this, &MainWindow::onListFiles);
    connect(m_downloadBtn, &QPushButton::clicked, this, &MainWindow::onDownload);
    connect(m_chunkUploadBtn, &QPushButton::clicked, this, &MainWindow::onChunkedUpload);
    connect(m_cancelUploadBtn, &QPushButton::clicked, this, &MainWindow::onCancelUpload);
    connect(m_dlResumeBtn, &QPushButton::clicked, this, &MainWindow::onResumableDownload);
    connect(m_clearLogBtn, &QPushButton::clicked, this, &MainWindow::onClearLog);

    QHBoxLayout *layout1 = new QHBoxLayout(row1);
    layout1->setContentsMargins(0, 0, 0, 0);
    layout1->addWidget(label);
    layout1->addWidget(m_serverEdit);
    layout1->addWidget(m_healthBtn);
    layout1->addWidget(m_uploadBtn);
    layout1->addWidget(m_listBtn);
    layout1->addWidget(m_downloadBtn);
    layout1->addWidget(m_chunkUploadBtn);
    layout1->addWidget(m_cancelUploadBtn);
    layout1->addWidget(m_dlResumeBtn);
    layout1->addWidget(m_clearLogBtn);
    layout1->addStretch(1);

    // 第二行：进度条 + 进度文案 + 结果标识（✓/✗ 简化反馈）
    QWidget *row2 = new QWidget(this);
    m_progressLabel = new QLabel(QStringLiteral("进度：空闲"), row2);
    m_progressBar = new QProgressBar(row2);
    m_progressBar->setRange(0, 1);
    m_progressBar->setValue(0);
    m_progressBar->setMinimumWidth(200);
    m_statusLabel = new QLabel(QString(), row2);   // 成功/失败的简化标识
    m_spaceLabel = new QLabel(QStringLiteral("服务器空间：查询中…"), row2);   // 剩余空间
    m_spaceLabel->setToolTip(QStringLiteral("来自 GET /api/v1/storage；每 60 秒自动刷新"));
    QHBoxLayout *layout2 = new QHBoxLayout(row2);
    layout2->setContentsMargins(0, 0, 0, 0);
    layout2->addWidget(m_progressLabel);
    layout2->addWidget(m_progressBar, 1);
    layout2->addWidget(m_statusLabel);
    layout2->addWidget(m_spaceLabel);

    barLayout->addWidget(row1);
    barLayout->addWidget(row2);
    return bar;
}

QWidget *MainWindow::createCenter()
{
    QWidget *box = new QWidget(this);

    // 表格列：ID / 名称 / 大小 / 时间 / Hash 前 8 位 / 是否秒传
    // ID 保留用于下载与预览取 id；原始 size 与 created_at 存进对应单元格的 UserRole
    m_table = new QTableWidget(0, 6, box);
    m_table->setHorizontalHeaderLabels(
        {QStringLiteral("ID"), QStringLiteral("名称"), QStringLiteral("大小"),
         QStringLiteral("时间"), QStringLiteral("Hash 前 8 位"), QStringLiteral("是否秒传")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);   // 支持多选下载
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setColumnWidth(0, 60);
    m_table->setColumnWidth(1, 200);
    m_table->setColumnWidth(2, 90);
    m_table->setColumnWidth(3, 150);
    m_table->setColumnWidth(4, 120);
    m_table->setColumnWidth(5, 90);
    // 选中行变化 -> 右侧侧边栏预览该行文件内容
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &MainWindow::onSelectionChanged);
    // 右键菜单：新建文件 / 重命名 / 删除 / 下载 / 上传到此目录 / 排序
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QTableWidget::customContextMenuRequested, this,
            &MainWindow::onTableContextMenu);
    // 点击表头也可排序（名称 / 大小 / 时间）
    m_table->horizontalHeader()->setSectionsClickable(true);
    connect(m_table->horizontalHeader(), &QHeaderView::sectionClicked, this, [this](int col) {
        int key = 0;
        if (col == 2) {
            key = 1;   // 大小
        } else if (col == 3) {
            key = 2;   // 时间
        } else {
            key = 0;   // 名称（其他列按名称）
        }
        applySort(key, m_sortKey == key ? !m_sortAsc : true);
    });

    // 左：文件列表  右：预览侧边栏，两者之间可拖拽调整宽度
    m_splitter = new QSplitter(Qt::Horizontal, box);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->addWidget(m_table);
    m_splitter->addWidget(createPreviewPanel());
    m_splitter->setStretchFactor(0, 3);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setSizes({620, 300});

    QVBoxLayout *layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_splitter);

    return box;
}

QWidget *MainWindow::createPreviewPanel()
{
    QWidget *panel = new QWidget(this);

    QLabel *caption = new QLabel(QStringLiteral("预览"), panel);
    QFont captionFont = caption->font();
    captionFont.setBold(true);
    caption->setFont(captionFont);

    m_previewName = new QLabel(QStringLiteral("（未选中文件）"), panel);
    m_previewName->setWordWrap(true);
    m_previewName->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_previewMeta = new QLabel(QStringLiteral("-"), panel);
    m_previewMeta->setWordWrap(true);
    m_previewMeta->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_previewText = new QPlainTextEdit(panel);
    m_previewText->setReadOnly(true);
    m_previewText->setLineWrapMode(QPlainTextEdit::NoWrap);   // 十六进制转储需要对齐
    m_previewText->setPlaceholderText(
        QStringLiteral("在左侧列表选中一行，这里会显示该文件的内容预览"));
    // 等宽字体：文本与十六进制预览都靠它对齐
    QFont fixedFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    fixedFont.setPointSize(10);
    m_previewText->setFont(fixedFont);

    // 超过预览上限时才出现，避免误拉大文件
    m_previewForceBtn = new QPushButton(QStringLiteral("仍要预览（可能很慢）"), panel);
    m_previewForceBtn->setVisible(false);
    connect(m_previewForceBtn, &QPushButton::clicked, this, &MainWindow::onForcePreview);

    QVBoxLayout *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(6, 0, 0, 0);
    layout->addWidget(caption);
    layout->addWidget(m_previewName);
    layout->addWidget(m_previewMeta);
    layout->addWidget(m_previewText, 1);
    layout->addWidget(m_previewForceBtn);

    return panel;
}

// ---------------------------------------------------------------------------
//  按钮槽函数
// ---------------------------------------------------------------------------
void MainWindow::onHealthCheck()
{
    const QUrl url = buildUrl(QStringLiteral("/healthz"));
    QNetworkRequest req(url);
    sendRequest(req, "GET");
}

void MainWindow::onUpload()
{
    const QStringList paths =
        QFileDialog::getOpenFileNames(this, QStringLiteral("选择要上传的文件（可多选）"));
    if (paths.isEmpty()) {
        return;   // 用户取消
    }

    // 自由选择上传位置：远端文件树对话框（可在其中【新建文件夹】）选目标目录
    FileTreeDialog dlg(buildUrl(QStringLiteral("/api/v1/dirs")),
                       buildUrl(QStringLiteral("/api/v1/files")),
                       FileTreeDialog::Mode::SelectDir, m_lastDir,
                       /*allowCreateDir=*/true, this);
    if (dlg.exec() != QDialog::Accepted) {
        return;   // 用户取消
    }
    const QString dir = dlg.selectedPath();   // 根目录 = 空串
    m_lastDir = dir;

    m_upQueue = paths;
    m_upDir = dir;
    m_upOk = 0;
    m_upFail = 0;
    m_upSpaceWarned = false;
    m_upBatch = true;
    showStatus(QStringLiteral("… 上传中 0/%1").arg(paths.size()), true);
    startNextUpload();
}

// 顺序上传：取队列首元素发整文件 POST；全部完成后给 ✓/✗ 汇总标识
void MainWindow::startNextUpload()
{
    if (m_upQueue.isEmpty()) {
        m_upBatch = false;
        showStatus(m_upFail == 0 ? QStringLiteral("✓ 上传成功 %1 个").arg(m_upOk)
                                 : QStringLiteral("✗ 成功 %1 / 失败 %2")
                                       .arg(m_upOk)
                                       .arg(m_upFail),
                   m_upFail == 0);
        refreshStorage();   // 上传占用了空间，刷新显示
        return;
    }
    const QString path = m_upQueue.takeFirst();
    const qint64 fileSize = QFileInfo(path).size();

    // 内存控制：大文件走分块上传（流式，5MiB/块），绝不 readAll() 整文件入内存
    if (fileSize > kStreamUploadThreshold) {
        appendLog(QStringLiteral("POST"), buildUrl(QStringLiteral("/api/v1/uploads/init")), 0, 0,
                  QStringLiteral("大文件 %1（%2 > %3）转分块流式上传")
                      .arg(path, formatSize(fileSize), formatSize(kStreamUploadThreshold)));
        QString err;
        if (!beginChunkedUploadFor(path, m_upDir, &err)) {
            if (m_scanning) {
                // 已有文件在扫描：排回队首，稍后重试（不算失败）
                m_upQueue.prepend(path);
                QTimer::singleShot(300, this, [this] { startNextUpload(); });
                return;
            }
            appendLog(QStringLiteral("init"), buildUrl(QStringLiteral("/api/v1/uploads/init")), 0, 0,
                      QStringLiteral("✗ 无法开始分块上传：%1（%2）").arg(path, err));
            ++m_upFail;
            QTimer::singleShot(0, this, [this] { startNextUpload(); });
        }
        return;   // 分块会话结束后由 finishChunkSession 推进队列
    }

    m_upCurrentPath = path;   // 同名冲突时用它重发
    sendWholeFile(path, false);
}

// 发送单个小文件（整文件 POST）。overwrite=true 时带 X-CV-Overwrite 覆盖同目录同名文件
void MainWindow::sendWholeFile(const QString &path, bool overwrite)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        appendLog(QStringLiteral("POST"), buildUrl(QStringLiteral("/api/v1/files")), 0, 0,
                  QStringLiteral("✗ 无法读取本机文件：%1（%2）").arg(path, file.errorString()));
        ++m_upFail;
        QTimer::singleShot(0, this, [this] { startNextUpload(); });
        return;
    }
    const QByteArray data = file.readAll();   // 仅小文件（≤ 8 MiB）
    file.close();
    const QString name = QFileInfo(path).fileName();

    QNetworkRequest req(buildUrl(QStringLiteral("/api/v1/files")));
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/octet-stream"));
    // 文件名按 RFC 3986 百分号编码后放进请求头，服务端会 urlDecode 还原
    req.setRawHeader("X-CV-Name", QUrl::toPercentEncoding(name));
    if (!m_upDir.isEmpty()) {
        req.setRawHeader("X-CV-Dir", QUrl::toPercentEncoding(m_upDir));
    }
    if (overwrite) {
        req.setRawHeader("X-CV-Overwrite", "1");
    }
    req.setHeader(QNetworkRequest::ContentLengthHeader, data.size());

    appendLog(QStringLiteral("POST"), req.url(), 0, 0,
              QStringLiteral("上传 %1（%2，目标目录=%3%4）")
                  .arg(path, formatSize(data.size()),
                       m_upDir.isEmpty() ? QStringLiteral("(根目录)") : m_upDir,
                       overwrite ? QStringLiteral("，覆盖同名") : QString()));
    sendRequest(req, "POST", data);
}

void MainWindow::onListFiles()
{
    QNetworkRequest req(buildUrl(QStringLiteral("/api/v1/files")));
    sendRequest(req, "GET");
}

void MainWindow::onDownload()
{
    // 多选下载：表格支持 ExtendedSelection，这里取全部选中行
    const QModelIndexList rows = m_table->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        QMessageBox::information(
            this, QStringLiteral("请先选择文件"),
            QStringLiteral("请在列表中选中一行或多行（Ctrl / Shift 多选），再点【下载选中文件（可多选）】。"));
        return;
    }

    QList<DlItem> picked;
    for (const QModelIndex &idx : rows) {
        DlItem it;
        it.id = m_table->item(idx.row(), 0) ? m_table->item(idx.row(), 0)->text() : QString();
        it.name = m_table->item(idx.row(), 1) ? m_table->item(idx.row(), 1)->text() : QString();
        const QTableWidgetItem *sizeCell = m_table->item(idx.row(), 2);
        it.size = sizeCell ? sizeCell->data(Qt::UserRole).toLongLong() : 0;
        if (it.name.isEmpty()) {
            it.name = it.id;
        }
        if (!it.id.isEmpty()) {
            picked.append(it);
        }
    }
    if (picked.isEmpty()) {
        return;
    }

    // 单个文件：先用"另存为"指定完整保存路径；多个文件：选一个保存目录，逐个落盘
    if (picked.size() == 1) {
        const QString savePath =
            QFileDialog::getSaveFileName(this, QStringLiteral("保存到"), picked.first().name);
        if (savePath.isEmpty()) {
            return;   // 用户取消
        }
        picked.first().targetPath = savePath;
        m_dlDir.clear();
        m_dlBatch = false;
    } else {
        const QString dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("选择保存目录（%1 个文件）").arg(picked.size()));
        if (dir.isEmpty()) {
            return;   // 用户取消
        }
        m_dlDir = dir;
        m_dlBatch = true;
    }

    m_dlQueue = picked;
    m_dlOk = 0;
    m_dlFail = 0;
    showStatus(QStringLiteral("… 下载中 0/%1").arg(picked.size()), true);
    startNextDownload();
}

// 顺序下载：**统一走 Range 分段**（每次 4MiB），内存占用与文件大小无关；
// 大文件不会一次性载入内存，中断后凭 .part 断点续传。全部完成后给 ✓/✗ 汇总标识
void MainWindow::startNextDownload()
{
    if (m_dlQueue.isEmpty()) {
        m_dlBatch = false;
        showStatus(m_dlFail == 0 ? QStringLiteral("✓ 下载成功 %1 个").arg(m_dlOk)
                                 : QStringLiteral("✗ 成功 %1 / 失败 %2")
                                       .arg(m_dlOk)
                                       .arg(m_dlFail),
                   m_dlFail == 0);
        return;
    }
    const DlItem item = m_dlQueue.takeFirst();
    const QString finalPath = item.targetPath.isEmpty()
        ? (m_dlDir + QLatin1Char('/') + item.name)
        : item.targetPath;
    const QString partPath = finalPath + QStringLiteral(".part");

    qint64 offset = 0;
    if (QFile::exists(partPath)) {
        offset = QFileInfo(partPath).size();   // 断点：已下载字节数 = .part 当前大小
    }

    m_dlActive = true;
    m_dlId = item.id;
    m_dlFinalPath = finalPath;
    m_dlPartPath = partPath;
    m_dlTotal = item.size;
    m_dlOffset = offset;

    appendLog(QStringLiteral("下载"),
              buildUrl(QStringLiteral("/api/v1/files/%1/content").arg(item.id)), 0, 0,
              QStringLiteral("开始分段下载 %1（%2）%3")
                  .arg(item.name, formatSize(item.size),
                       offset > 0 ? QStringLiteral("，从 offset=%1 续传").arg(offset) : QString()));

    sendDownloadRange(offset);
}

// ---------------------------------------------------------------------------
//  文件列表右键菜单：新建文件 / 重命名 / 删除 / 下载 / 上传到此目录 / 排序
// ---------------------------------------------------------------------------
void MainWindow::onTableContextMenu(const QPoint &pos)
{
    const QTableWidgetItem *cell = m_table->itemAt(pos);
    const int row = cell ? cell->row() : -1;
    if (row >= 0) {
        m_table->selectRow(row);   // 右键先选中该行
    }
    const QString id = (row >= 0 && m_table->item(row, 0))
        ? m_table->item(row, 0)->data(Qt::UserRole).toString()
        : QString();
    const QString name = (row >= 0 && m_table->item(row, 1))
        ? m_table->item(row, 1)->text()
        : QString();
    // 该行所属目录（无行时为根目录，供"新建文件/上传到此目录"用）
    m_ctxDir = (row >= 0 && m_table->item(row, 1))
        ? m_table->item(row, 1)->data(Qt::UserRole + 1).toString()
        : QString();

    QMenu menu(this);
    QAction *aNew = menu.addAction(QStringLiteral("新建文件"));
    QAction *aRename = menu.addAction(QStringLiteral("重命名"));
    QAction *aDelete = menu.addAction(QStringLiteral("删除"));
    menu.addSeparator();
    QAction *aDownload = menu.addAction(QStringLiteral("下载"));
    QAction *aUpload = menu.addAction(QStringLiteral("上传到此目录"));
    menu.addSeparator();
    QMenu *sortMenu = menu.addMenu(QStringLiteral("排序"));
    QAction *sName = sortMenu->addAction(QStringLiteral("按名称"));
    QAction *sSize = sortMenu->addAction(QStringLiteral("按大小"));
    QAction *sTime = sortMenu->addAction(QStringLiteral("按时间"));
    sortMenu->addSeparator();
    QAction *sAsc = sortMenu->addAction(QStringLiteral("升序"));
    QAction *sDesc = sortMenu->addAction(QStringLiteral("降序"));

    const bool hasRow = !id.isEmpty();
    aRename->setEnabled(hasRow);
    aDelete->setEnabled(hasRow);
    aDownload->setEnabled(hasRow);
    if (hasRow) {
        aRename->setText(QStringLiteral("重命名「%1」").arg(name));
        aDelete->setText(QStringLiteral("删除「%1」").arg(name));
    }

    QAction *picked = menu.exec(m_table->viewport()->mapToGlobal(pos));
    if (!picked) {
        return;
    }
    if (picked == aNew) {
        onCreateFile();
    } else if (picked == aRename) {
        onRenameFile();
    } else if (picked == aDelete) {
        onDeleteFile();
    } else if (picked == aDownload) {
        onDownload();          // 复用多选下载（右键已选中该行）
    } else if (picked == aUpload) {
        onUploadToDir();       // 上传到该行所在目录
    } else if (picked == sName) {
        applySort(0, true);
    } else if (picked == sSize) {
        applySort(1, false);
    } else if (picked == sTime) {
        applySort(2, false);
    } else if (picked == sAsc) {
        applySort(m_sortKey, true);
    } else if (picked == sDesc) {
        applySort(m_sortKey, false);
    }
}

// 新建空文件（落在右键行所在目录；没右键到行时落在根目录）
void MainWindow::onCreateFile()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("新建文件"),
                                               QStringLiteral("文件名（单段，不含 '/'）："),
                                               QLineEdit::Normal, QString(), &ok)
                             .trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }
    QString why;
    if (!validRelPathInput(name, &why) || name.contains(QLatin1Char('/'))) {
        QMessageBox::warning(this, QStringLiteral("文件名不合法"),
                             why.isEmpty() ? QStringLiteral("文件名不能包含 '/'") : why);
        return;
    }
    QJsonObject body;
    body.insert(QStringLiteral("name"), name);
    body.insert(QStringLiteral("dir"), m_ctxDir);
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    QNetworkRequest req(buildUrl(QStringLiteral("/api/v1/files/new")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setHeader(QNetworkRequest::ContentLengthHeader, payload.size());
    QNetworkReply *r = sendRequest(req, "POST", payload);
    r->setProperty("cvStep", QStringLiteral("newfile"));
}

// 重命名选中文件
void MainWindow::onRenameFile()
{
    const QString id = currentFileId();
    if (id.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("请先选择文件"),
                                 QStringLiteral("请选中一行后再重命名。"));
        return;
    }
    const QString oldName = currentFileName();
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("重命名"),
                                               QStringLiteral("新名称（单段，不含 '/'）："),
                                               QLineEdit::Normal, oldName, &ok)
                             .trimmed();
    if (!ok || name.isEmpty() || name == oldName) {
        return;
    }
    QString why;
    if (!validRelPathInput(name, &why) || name.contains(QLatin1Char('/'))) {
        QMessageBox::warning(this, QStringLiteral("名称不合法"),
                             why.isEmpty() ? QStringLiteral("名称不能包含 '/'") : why);
        return;
    }
    QJsonObject body;
    body.insert(QStringLiteral("name"), name);
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    QNetworkRequest req(buildUrl(QStringLiteral("/api/v1/files/%1/rename").arg(id)));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setHeader(QNetworkRequest::ContentLengthHeader, payload.size());
    QNetworkReply *r = sendRequest(req, "POST", payload);
    r->setProperty("cvStep", QStringLiteral("rename"));
}

// 删除选中文件（危险操作：二次确认）
void MainWindow::onDeleteFile()
{
    const QString id = currentFileId();
    if (id.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("请先选择文件"),
                                 QStringLiteral("请选中一行后再删除。"));
        return;
    }
    const QString name = currentFileName();
    const auto ret = QMessageBox::question(
        this, QStringLiteral("确认删除"),
        QStringLiteral("确定删除「%1」吗？此操作不可撤销。").arg(name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ret != QMessageBox::Yes) {
        return;
    }
    QNetworkRequest req(buildUrl(QStringLiteral("/api/v1/files/%1").arg(id)));
    QNetworkReply *r = sendRequest(req, "DELETE");
    r->setProperty("cvStep", QStringLiteral("delete"));
}

// 上传到右键行所在目录（复用批量上传流程，仅把目标目录固定为 m_ctxDir）
void MainWindow::onUploadToDir()
{
    const QString where = m_ctxDir.isEmpty() ? QStringLiteral("根目录") : m_ctxDir;
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, QStringLiteral("选择要上传到「%1」的文件（可多选）").arg(where));
    if (paths.isEmpty()) {
        return;
    }
    m_upQueue = paths;
    m_upDir = m_ctxDir;
    m_lastDir = m_ctxDir;
    m_upOk = 0;
    m_upFail = 0;
    m_upSpaceWarned = false;
    m_upBatch = true;
    showStatus(QStringLiteral("… 上传中 0/%1").arg(paths.size()), true);
    startNextUpload();
}

// ---- 新建 / 重命名 / 删除 的响应处理 ----

void MainWindow::handleNewFileReply(int status, const QByteArray &raw)
{
    if (status == 201) {
        appendLog(QStringLiteral("新建文件"), buildUrl(QStringLiteral("/api/v1/files/new")),
                  status, 0, formatBody(raw));
        showStatus(QStringLiteral("✓ 新建文件成功"), true);
        onListFiles();   // 刷新列表
        return;
    }
    if (status == 409) {
        const QJsonObject o = QJsonDocument::fromJson(raw).object();
        QMessageBox::warning(this, QStringLiteral("文件已存在"),
                             QStringLiteral("该目录下已存在同名文件「%1」，请换个名字。")
                                 .arg(o.value(QStringLiteral("name")).toString()));
        return;
    }
    showStatus(QStringLiteral("✗ 新建文件失败"), false);
    QMessageBox::warning(this, QStringLiteral("新建文件失败"),
                         QStringLiteral("HTTP %1：%2").arg(status).arg(formatBody(raw)));
}

void MainWindow::handleRenameReply(int status, const QByteArray &raw)
{
    if (status == 200) {
        appendLog(QStringLiteral("重命名"), buildUrl(QStringLiteral("/api/v1/files")), status, 0,
                  formatBody(raw));
        showStatus(QStringLiteral("✓ 重命名成功"), true);
        onListFiles();
        return;
    }
    if (status == 409) {
        const QJsonObject o = QJsonDocument::fromJson(raw).object();
        QMessageBox::warning(this, QStringLiteral("名称冲突"),
                             QStringLiteral("该目录下已存在「%1」，请换个名字。")
                                 .arg(o.value(QStringLiteral("name")).toString()));
        return;
    }
    showStatus(QStringLiteral("✗ 重命名失败"), false);
    QMessageBox::warning(this, QStringLiteral("重命名失败"),
                         QStringLiteral("HTTP %1：%2").arg(status).arg(formatBody(raw)));
}

void MainWindow::handleDeleteReply(int status, const QByteArray &raw)
{
    if (status == 200 || status == 204) {
        qint64 freed = 0;
        if (status == 200) {
            const QJsonObject o = QJsonDocument::fromJson(raw).object();
            freed = o.value(QStringLiteral("freed_bytes")).toVariant().toLongLong();
        }
        appendLog(QStringLiteral("删除"), buildUrl(QStringLiteral("/api/v1/files")), status, 0,
                  QStringLiteral("✓ 已删除（释放 %1）").arg(formatSize(freed)));
        showStatus(freed > 0 ? QStringLiteral("✓ 删除成功，释放 %1").arg(formatSize(freed))
                             : QStringLiteral("✓ 删除成功"),
                   true);
        onListFiles();
        refreshStorage();   // 空间变化了，刷新显示
        return;
    }
    showStatus(QStringLiteral("✗ 删除失败"), false);
    QMessageBox::warning(this, QStringLiteral("删除失败"),
                         QStringLiteral("HTTP %1：%2").arg(status).arg(formatBody(raw)));
}

// 同名冲突询问：true = 覆盖，false = 跳过
bool MainWindow::askOverwrite(const QString &dir, const QString &name)
{
    const QString where = dir.isEmpty() ? QStringLiteral("根目录") : dir;
    const auto ret = QMessageBox::question(
        this, QStringLiteral("同名文件已存在"),
        QStringLiteral("「%1」目录下已存在同名文件「%2」。\n是否覆盖？").arg(where, name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    return ret == QMessageBox::Yes;
}

void MainWindow::onChunkedUpload()
{
    if (m_chunkActive) {
        QMessageBox::information(this, QStringLiteral("正在上传"),
                                 QStringLiteral("已有分块上传会话进行中，请先等待完成或点击【取消上传】。"));
        return;
    }

    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择要分块上传的文件"));
    if (path.isEmpty()) {
        return;   // 用户取消
    }

    // 目标目录：用远端文件树对话框选（相对路径；根目录 = 空串）
    FileTreeDialog dlg(buildUrl(QStringLiteral("/api/v1/dirs")),
                       buildUrl(QStringLiteral("/api/v1/files")),
                       FileTreeDialog::Mode::SelectDir, m_lastDir,
                       /*allowCreateDir=*/false, this);
    if (dlg.exec() != QDialog::Accepted) {
        return;   // 用户取消
    }
    const QString dir = dlg.selectedPath();   // 根目录 = 空串
    m_lastDir = dir;

    QString err;
    if (!beginChunkedUploadFor(path, dir, &err)) {
        QMessageBox::warning(this, QStringLiteral("无法开始分块上传"), err);
    }
}

// 建立一次分块上传会话（流式：按 5MiB 逐块 seek+read，绝不整文件入内存）。
// 供【分块上传】按钮与批量上传中的大文件共用；失败时返回 false 且 err 非空。
// 纯函数：流式扫描文件 + 算整文件 SHA-256（定义在文件后部；在线程池里执行）
ChunkScanResult scanFileForChunks(const QString &path);

bool MainWindow::beginChunkedUploadFor(const QString &path, const QString &dir, QString *err)
{
    if (m_chunkActive) {
        if (err) {
            *err = QStringLiteral("已有分块上传会话进行中");
        }
        return false;
    }
    if (m_scanning) {
        if (err) {
            *err = QStringLiteral("正在扫描另一个文件，请稍候");
        }
        return false;
    }

    // 大文件的扫描 + SHA-256 可能耗时数十秒：放到线程池，避免冻结界面
    m_scanning = true;
    m_pendingChunkDir = dir;
    showStatus(QStringLiteral("… 正在扫描文件（计算 SHA-256）…"), true);
    QThreadPool *pool = m_ioPool;
    QPointer<MainWindow> self(this);
    pool->start( [self, path]() {
        const ChunkScanResult r = scanFileForChunks(path);
        if (self) {
            QMetaObject::invokeMethod(
                self, [self, r]() { if (self) self->onChunkScanDone(r); }, Qt::QueuedConnection);
        }
    });
    return true;
}

// 扫描（含整文件 SHA-256）在线程池里完成 → 回主线程继续建立上传会话
void MainWindow::onChunkScanDone(const ChunkScanResult &r)
{
    m_scanning = false;
    if (!r.ok) {
        appendLog(QStringLiteral("扫描"), buildUrl(QStringLiteral("/api/v1/uploads/init")), 0, 0,
                  QStringLiteral("✗ 无法读取本机文件：%1（%2）").arg(r.path, r.err));
        showStatus(QStringLiteral("✗ 扫描文件失败"), false);
        if (m_upBatch) {
            ++m_upFail;
            QTimer::singleShot(0, this, [this] { startNextUpload(); });
        }
        return;
    }
    m_chunkFileSize = r.size;
    m_chunkMtime = r.mtime;
    m_chunkName = r.name;
    m_chunkPath = r.path;
    m_chunkFileHash = r.hash;
    m_chunks = r.chunks;
    afterChunkPlanReady(m_pendingChunkDir);
}

// 拿到分块表后：重置会话状态、尝试续传、启动 init
void MainWindow::afterChunkPlanReady(const QString &dir)
{
    m_chunkDir = dir;
    // 本地即可算出总分块数（chunk_size 固定为 kChunkSize），保证所有分支（含 GET 会话续传）
    // 在传块前都已知道 m_totalChunks
    m_totalChunks = static_cast<int>((m_chunkFileSize + kChunkSize - 1) / kChunkSize);

    // 重置会话（manifest 命中与否在 loadManifest 中决定）
    m_uploadId = 0;
    m_doneSeq.clear();
    m_inflightSeq.clear();
    m_chunkRetries.clear();
    m_resumeHit = false;
    m_completeSent = false;
    m_cancelRequested = false;
    m_chunkOverwrite = false;   // 每次新会话重新询问同名覆盖

    // 本地 manifest 校验：size / mtime / full_hash 三者全一致才复用，否则从头传
    const bool resumed = loadManifest(m_chunkPath);
    if (!resumed) {
        m_uploadId = 0;
        m_doneSeq.clear();
        m_resumeHit = false;
    }

    m_chunkActive = true;
    updateCancelButton();
    updateProgress();

    if (resumed && m_uploadId > 0) {
        // 先确认 upload_id 是否仍有效（服务端会话被清则 404 -> 重新 init）
        appendLog(QStringLiteral("续传"), buildUrl(QStringLiteral("/api/v1/uploads")), 0, 0,
                  QStringLiteral("命中本地 manifest，尝试复用 upload_id=%1，已确认分块 %2 块")
                      .arg(m_uploadId).arg(m_doneSeq.size()));
        sendGetSession();
    } else {
        sendInit();
    }
}

void MainWindow::onCancelUpload()
{
    if (!m_chunkActive) {
        return;
    }
    m_cancelRequested = true;
    appendLog(QStringLiteral("取消"), buildUrl(QStringLiteral("/api/v1/uploads")), 0, 0,
              QStringLiteral("用户取消，请求服务端丢弃会话 upload_id=%1").arg(m_uploadId));

    // 通知服务端丢弃会话（可选，但契约支持）
    if (m_uploadId > 0) {
        QNetworkRequest req(buildUrl(QStringLiteral("/api/v1/uploads/%1").arg(m_uploadId)));
        QNetworkReply *r = sendRequest(req, "DELETE");
        r->setProperty("cvStep", QStringLiteral("cancel"));
    }

    finishChunkSession(false);   // 保留 upload_id 与已传分块，供下次续传
}

void MainWindow::onResumableDownload()
{
    if (m_dlActive) {
        QMessageBox::information(this, QStringLiteral("正在下载"),
                                 QStringLiteral("已有分块下载进行中。"));
        return;
    }

    const QString id = currentFileId();
    if (id.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("请先选择文件"),
                                 QStringLiteral("请在列表中选中一行，再点击【分块下载（断点续传）】。"));
        return;
    }

    const QString name = currentFileName();
    const QString savePath = QFileDialog::getSaveFileName(this, QStringLiteral("保存到"), name);
    if (savePath.isEmpty()) {
        return;   // 用户取消
    }

    const QString partPath = savePath + QStringLiteral(".part");
    qint64 offset = 0;
    if (QFile::exists(partPath)) {
        offset = QFileInfo(partPath).size();   // 已下载字节数 = .part 当前大小
    }

    m_dlActive = true;
    m_dlId = id;
    m_dlFinalPath = savePath;
    m_dlPartPath = partPath;
    m_dlTotal = currentFileSize();   // 来自列表，最终以 Content-Range 的 total 为准
    m_dlOffset = offset;

    QUrl url = buildUrl(QStringLiteral("/api/v1/files/%1/content").arg(id));
    if (offset > 0) {
        appendLog(QStringLiteral("下载"), url, 0, 0,
                  QStringLiteral("续传：已存在 .part（%1），从 offset=%2 继续")
                      .arg(partPath).arg(offset));
    }

    sendDownloadRange(offset);
}

void MainWindow::onClearLog()
{
    m_log->clear();
}

void MainWindow::onSelectionChanged()
{
    // 切换选中行时先丢弃上一次未完成的预览，否则快速点选会让结果错乱
    cancelPreview();
    m_previewForceBtn->setVisible(false);

    const QString id = currentFileId();
    if (id.isEmpty()) {
        m_previewId.clear();
        resetPreview();
        return;
    }

    const QString name = currentFileName();
    const qint64 size = currentFileSize();
    const qint64 createdAt = currentFileCreatedAt();
    m_previewId = id;
    setPreviewHeader(name, size, createdAt);

    // 体积保护：超过 16 MiB 不自动拉取，交给用户显式确认
    if (size > kMaxAutoPreviewBytes) {
        m_previewText->setPlainText(
            QStringLiteral("文件 %1（%2）超过预览上限（%3），请用【下载选中文件】查看。\n\n"
                           "确认要拉取的话，点下方的【仍要预览】。")
                .arg(name, formatSize(size), formatSize(kMaxAutoPreviewBytes)));
        m_previewForceBtn->setVisible(true);
        return;
    }

    startPreview(id);
}

void MainWindow::onForcePreview()
{
    m_previewForceBtn->setVisible(false);
    if (m_previewId.isEmpty()) {
        return;
    }
    startPreview(m_previewId);
}

// ---------------------------------------------------------------------------
//  预览
// ---------------------------------------------------------------------------
void MainWindow::cancelPreview()
{
    // 先摘指针、再置空、最后中止：abort() 会同步派发 finished()，
    // onReplyFinished / handleReply 里会把 m_previewReply 置空并 deleteLater()，
    // 若返回后再解引用原指针就是空指针崩溃（快速点选必然踩到）
    QNetworkReply *reply = m_previewReply;
    m_previewReply = nullptr;
    if (reply == nullptr) {
        return;
    }
    reply->abort();
    reply->deleteLater();
}

void MainWindow::startPreview(const QString &id)
{
    cancelPreview();
    m_previewId = id;
    m_previewText->setPlainText(QStringLiteral("正在加载预览…"));

    QNetworkRequest req(buildUrl(QStringLiteral("/api/v1/files/%1/content").arg(id)));
    QNetworkReply *reply = sendRequest(req, "GET");
    // 打动态属性：下载与预览走同一个 URL，靠它区分，避免被当成下载落盘
    reply->setProperty("cvPreview", true);
    m_previewReply = reply;
}

void MainWindow::handlePreviewReply(int status, const QString &errorString, bool networkError,
                                    const QByteArray &raw)
{
    if (networkError) {
        // 404/500 之类的 HTTP 错误 Qt 同样会置 error()，所以这里把状态码一起打出来，
        // 否则只看得到 errorString、看不出是 404 还是 500
        const QString detail = errorString.isEmpty() ? QStringLiteral("（无错误信息）")
                                                     : errorString;
        m_previewText->setPlainText(status > 0
            ? QStringLiteral("预览失败：HTTP %1：%2").arg(QString::number(status), detail)
            : QStringLiteral("预览失败：%1").arg(detail));
        return;
    }
    if (status != 200) {
        m_previewText->setPlainText(
            QStringLiteral("预览失败：HTTP %1\n%2").arg(QString::number(status),
                                                        errorString.isEmpty()
                                                            ? QStringLiteral("（无错误信息）")
                                                            : errorString));
        return;
    }
    renderPreview(raw);
}

void MainWindow::renderPreview(const QByteArray &data)
{
    if (data.isEmpty()) {
        m_previewText->setPlainText(QStringLiteral("（空文件）"));
        return;
    }

    const bool truncated = data.size() > kMaxPreviewRenderBytes;
    QByteArray slice = truncated ? data.first(kMaxPreviewRenderBytes) : data;
    const QString note = truncated
        ? QStringLiteral("\n\n（共 %1 字节，仅预览前 %2）")
              .arg(QString::number(static_cast<qint64>(data.size())),
                   formatSize(kMaxPreviewRenderBytes))
        : QString();

    // 截断可能切在多字节 UTF-8 序列中间（中文约 2/3 概率），半个汉字会让整段变成
    // 非法 UTF-8 从而被误判成二进制。先把切点回退到字符边界（最多 3 字节）再判定；
    // 回退后仍非法，说明本来就真是二进制，交回 looksBinary 处理
    if (truncated) {
        for (int i = 0; i < 3 && !slice.isEmpty()
             && QString::fromUtf8(slice).toUtf8() != slice; ++i) {
            slice.chop(1);
        }
    }

    if (looksBinary(slice)) {
        const QByteArray head = slice.first(kMaxHexDumpBytes);
        QString text = QStringLiteral("（二进制文件，以下为前 %1 字节的十六进制预览）\n\n")
                           .arg(QString::number(static_cast<qint64>(head.size())));
        text += toHexDump(head);
        m_previewText->setPlainText(text + note);
        return;
    }

    // 切点已在上面回退到字符边界，这里可以直接解码
    m_previewText->setPlainText(QString::fromUtf8(slice) + note);
}

void MainWindow::setPreviewHeader(const QString &name, qint64 size, qint64 createdAt)
{
    m_previewName->setText(name.isEmpty() ? QStringLiteral("（未命名）") : name);
    m_previewName->setToolTip(name);
    m_previewMeta->setText(QStringLiteral("大小：%1    时间：%2")
                               .arg(formatSize(size), formatTime(createdAt)));
}

void MainWindow::resetPreview()
{
    m_previewName->setText(QStringLiteral("（未选中文件）"));
    m_previewName->setToolTip(QString());
    m_previewMeta->setText(QStringLiteral("-"));
    m_previewText->setPlainText(QString());
}

// ---------------------------------------------------------------------------
//  分块上传（断点续传）
// ---------------------------------------------------------------------------

// 流式读取文件，边读边喂 QCryptographicHash 算整文件 SHA-256，
// 同时按 chunk_size 切出每个分块的偏移 / 长度表（任何时刻内存里只有 1MiB 缓冲）。
// 流式扫描本地文件：边读边算整文件 SHA-256，同时按 kChunkSize 切出分块表。
// ⚠️ 纯函数、不碰 Qt 界面对象 —— 它在线程池里执行（大文件扫描+哈希是 UI 卡顿的元凶）。
ChunkScanResult scanFileForChunks(const QString &path)
{
    ChunkScanResult r;
    QFileInfo info(path);
    r.size = info.size();
    r.mtime = info.lastModified().toMSecsSinceEpoch();
    r.name = info.fileName();
    r.path = info.absoluteFilePath();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        r.err = file.errorString();
        return r;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 chunkStart = 0;   // 当前分块的起始偏移
    qint64 curLen = 0;       // 当前分块已累积长度
    qint64 pos = 0;          // 已处理的总字节数
    const qint64 size = r.size;

    QByteArray buf;
    buf.resize(static_cast<int>(kHashBufferSize));

    while (pos < size) {
        const qint64 toRead = qMin(kHashBufferSize, size - pos);
        const qint64 got = file.read(buf.data(), static_cast<int>(toRead));
        if (got != toRead || got < 0) {
            r.err = QStringLiteral("读取文件失败");
            file.close();
            return r;
        }
        hash.addData(QByteArrayView(buf.constData(), static_cast<qsizetype>(got)));

        qsizetype used = 0;
        while (used < static_cast<qsizetype>(got)) {
            const qsizetype room = static_cast<qsizetype>(kChunkSize - curLen);
            const qsizetype take = qMin(room, static_cast<qsizetype>(got) - used);
            curLen += take;
            used += take;
            pos += take;
            // 分块攒满，或已到文件末尾 -> 收尾一个分块
            if (curLen >= kChunkSize || pos >= size) {
                r.chunks.append(ChunkPlan{chunkStart, curLen});
                chunkStart = pos;
                curLen = 0;
            }
        }
    }

    file.close();
    r.hash = QString::fromUtf8(hash.result().toHex());
    r.ok = true;
    return r;
}

// 读取本地 manifest，仅当 size/mtime/full_hash 与当前文件完全一致才视为命中续传
bool MainWindow::loadManifest(const QString &path)
{
    const QString file = m_manifestDir + QStringLiteral("/upload_manifest.json");
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject()) {
        return false;
    }
    const QJsonObject entry = doc.object().value(path).toObject();
    if (entry.isEmpty()) {
        return false;
    }
    const qint64 size = entry.value(QStringLiteral("size")).toVariant().toLongLong();
    const qint64 mtime = entry.value(QStringLiteral("mtime")).toVariant().toLongLong();
    const QString hash = entry.value(QStringLiteral("full_hash")).toString();
    if (size != m_chunkFileSize || mtime != m_chunkMtime || hash != m_chunkFileHash) {
        return false;   // 源文件已变化，废弃该 manifest（从头传）
    }
    m_uploadId = entry.value(QStringLiteral("upload_id")).toVariant().toLongLong();
    m_doneSeq.clear();
    for (const QJsonValue &v : entry.value(QStringLiteral("uploaded_seqs")).toArray()) {
        m_doneSeq.insert(v.toInt());
    }
    return true;
}

// 把当前会话（upload_id + 已确认分块）落盘，保证程序退出也能续传
void MainWindow::saveManifest()
{
    const QString file = m_manifestDir + QStringLiteral("/upload_manifest.json");
    QDir().mkpath(m_manifestDir);

    QJsonObject root;
    {
        QFile f(file);
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
            if (doc.isObject()) {
                root = doc.object();
            }
            f.close();
        }
    }

    QJsonObject entry;
    entry.insert(QStringLiteral("abs_path"), m_chunkPath);
    entry.insert(QStringLiteral("name"), m_chunkName);
    entry.insert(QStringLiteral("size"), m_chunkFileSize);
    entry.insert(QStringLiteral("mtime"), m_chunkMtime);
    entry.insert(QStringLiteral("full_hash"), m_chunkFileHash);
    entry.insert(QStringLiteral("upload_id"), m_uploadId);
    entry.insert(QStringLiteral("chunk_size"), kChunkSize);
    QJsonArray uploaded;
    for (int s : m_doneSeq) {
        uploaded.append(s);
    }
    entry.insert(QStringLiteral("uploaded_seqs"), uploaded);
    root.insert(m_chunkPath, entry);

    QFile f(file);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        f.close();
    }
}

// 上传完成后删除该文件的 manifest 记录
void MainWindow::clearManifest()
{
    const QString file = m_manifestDir + QStringLiteral("/upload_manifest.json");
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) {
        return;
    }
    QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    if (root.isEmpty()) {
        return;
    }
    root.remove(m_chunkPath);
    if (root.isEmpty()) {
        f.remove();
        return;
    }
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        f.close();
    }
}

void MainWindow::sendGetSession()
{
    QNetworkRequest req(buildUrl(QStringLiteral("/api/v1/uploads/%1").arg(m_uploadId)));
    QNetworkReply *r = sendRequest(req, "GET");
    r->setProperty("cvStep", QStringLiteral("getsession"));
}

void MainWindow::sendInit()
{
    QJsonObject body;
    body.insert(QStringLiteral("name"), m_chunkName);
    body.insert(QStringLiteral("size"), m_chunkFileSize);
    body.insert(QStringLiteral("chunk_size"), kChunkSize);
    body.insert(QStringLiteral("hash"), m_chunkFileHash);
    body.insert(QStringLiteral("dir"), m_chunkDir);   // '' = 根目录；非法/越界服务端 400/403
    body.insert(QStringLiteral("overwrite"), m_chunkOverwrite);   // 同名覆盖（用户已确认）
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QNetworkRequest req(buildUrl(QStringLiteral("/api/v1/uploads/init")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setHeader(QNetworkRequest::ContentLengthHeader, payload.size());
    QNetworkReply *r = sendRequest(req, "POST", payload);
    r->setProperty("cvStep", QStringLiteral("init"));
    appendLog(QStringLiteral("init"), req.url(), 0, 0,
              QStringLiteral("准备分块上传：%1（%2，目标目录=%3，chunk_size=%4，hash=%5）")
                  .arg(m_chunkPath, formatSize(m_chunkFileSize),
                       m_chunkDir.isEmpty() ? QStringLiteral("(根目录)") : m_chunkDir,
                       formatSize(kChunkSize), m_chunkFileHash));
}

// 合并"服务端已传分块"与"本地 manifest 已传分块"，落盘，然后启动并发上传
void MainWindow::beginUploading(const QJsonArray &serverUploaded)
{
    if (!m_chunkActive) {
        return;
    }
    for (const QJsonValue &v : serverUploaded) {
        m_doneSeq.insert(v.toInt());
    }
    saveManifest();
    updateProgress();

    if (m_resumeHit && !m_doneSeq.isEmpty()) {
        appendLog(QStringLiteral("续传"), buildUrl(QStringLiteral("/api/v1/uploads")), 0, 0,
                  QStringLiteral("已确认分块 %1/%2，将从断点继续（跳过已传分块）")
                      .arg(m_doneSeq.size()).arg(m_totalChunks));
    }
    pumpChunks();
}

// 在并发上限内补齐缺失分块；全部完成则请求 complete
void MainWindow::pumpChunks()
{
    if (!m_chunkActive) {
        return;
    }
    for (int seq = 0; seq < m_totalChunks; ++seq) {
        if (!m_chunkActive) {
            return;   // launchChunk 可能因本地读取失败而终止会话，立即停止补块
        }
        if (m_inflightSeq.contains(seq) || m_doneSeq.contains(seq)) {
            continue;
        }
        if (m_inflightSeq.size() >= kMaxConcurrent) {
            break;
        }
        launchChunkAsync(seq);
    }

    const bool allDone = (m_totalChunks == 0) ||
        (m_doneSeq.size() >= m_totalChunks && m_inflightSeq.isEmpty());
    if (allDone && !m_completeSent) {
        m_completeSent = true;
        sendComplete();
    }
}

// 在**线程池**里读取单个分块（5MiB seek+read 不该阻塞 UI），读完回主线程发送
void MainWindow::launchChunkAsync(int seq)
{
    if (seq < 0 || seq >= m_chunks.size()) {
        return;
    }
    // 先占位，避免 pumpChunks 重复派发同一块
    m_inflightSeq.insert(seq);

    const QString path = m_chunkPath;
    const ChunkPlan plan = m_chunks.at(seq);
    QThreadPool *pool = m_ioPool;
    QPointer<MainWindow> self(this);
    pool->start( [self, path, plan, seq]() {
        ChunkReadResult r;
        r.seq = seq;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            r.err = file.errorString();
        } else if (!file.seek(plan.offset)) {
            r.err = QStringLiteral("seek 失败");
            file.close();
        } else {
            r.data = file.read(plan.length);
            file.close();
            if (r.data.size() != plan.length) {
                r.err = QStringLiteral("读取文件失败（期望 %1 字节，实得 %2）")
                            .arg(plan.length).arg(r.data.size());
            } else {
                // 分块 SHA-256 在线程池里算（此前在 UI 线程，5MiB 一块会造成卡顿）
                r.hash = QString::fromUtf8(
                    QCryptographicHash::hash(r.data, QCryptographicHash::Sha256).toHex());
                r.ok = true;
            }
        }
        if (self) {
            QMetaObject::invokeMethod(
                self, [self, r]() { if (self) self->onChunkReadDone(r); }, Qt::QueuedConnection);
        }
    });
}

void MainWindow::onChunkReadDone(const ChunkReadResult &r)
{
    if (!m_chunkActive) {
        return;   // 会话已结束/取消：丢弃在途结果
    }
    if (m_cancelRequested) {
        m_inflightSeq.remove(r.seq);
        return;
    }
    if (!r.ok) {
        m_inflightSeq.remove(r.seq);
        appendLog(QStringLiteral("chunk"),
                  buildUrl(QStringLiteral("/api/v1/uploads/%1/chunk/%2").arg(m_uploadId).arg(r.seq)),
                  0, 0, QStringLiteral("[读取失败] %1").arg(r.err));
        finishChunkSession(false);
        return;
    }

    const QString chunkHash = r.hash;   // 已在线程池内算好
    QNetworkRequest req(buildUrl(QStringLiteral("/api/v1/uploads/%1/chunk/%2")
                                     .arg(m_uploadId).arg(r.seq)));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
    req.setHeader(QNetworkRequest::ContentLengthHeader, r.data.size());
    req.setRawHeader("X-Chunk-SHA256", chunkHash.toUtf8());
    QNetworkReply *reply = sendRequest(req, "PUT", r.data);
    reply->setProperty("cvStep", QStringLiteral("chunk"));
    reply->setProperty("cvSeq", r.seq);
}

void MainWindow::sendComplete()
{
    QNetworkRequest req(buildUrl(QStringLiteral("/api/v1/uploads/%1/complete").arg(m_uploadId)));
    QNetworkReply *r = sendRequest(req, "POST", QByteArray());
    r->setProperty("cvStep", QStringLiteral("complete"));
    appendLog(QStringLiteral("complete"), req.url(), 0, 0,
              QStringLiteral("所有分块已传完，请求合并（upload_id=%1）").arg(m_uploadId));
}

// 分块会话响应统一收口（init / getsession / chunk / complete / cancel）
void MainWindow::handleChunkedReply(const QString &step, int seq, int status, bool networkError,
                                    const QString &errorString, const QByteArray &raw,
                                    qint64 dlTotal, qint64 dlStart)
{
    if (step == QStringLiteral("dlchunk")) {
        // 分块下载必须在取消守卫之前分派：下载会话与上传会话互不相干
        handleDownloadChunkReply(status, networkError, errorString, raw, dlTotal, dlStart);
        return;
    }
    if (step == QStringLiteral("storage")) {
        applyStorageInfo(raw);
        return;
    }
    if (step == QStringLiteral("newfile")) {
        handleNewFileReply(status, raw);
        return;
    }
    if (step == QStringLiteral("rename")) {
        handleRenameReply(status, raw);
        return;
    }
    if (step == QStringLiteral("delete")) {
        handleDeleteReply(status, raw);
        return;
    }
    if (m_cancelRequested && step != QStringLiteral("cancel")) {
        return;   // 取消中的在途请求直接丢弃
    }
    if (step == QStringLiteral("getsession")) {
        handleGetSessionReply(status, raw);
    } else if (step == QStringLiteral("init")) {
        handleInitReply(status, raw);
    } else if (step == QStringLiteral("chunk")) {
        handleChunkReply(status, errorString, raw, seq);
    } else if (step == QStringLiteral("complete")) {
        handleCompleteReply(status, errorString, raw);
    }
    // cancel 步骤无需额外处理
}

void MainWindow::handleGetSessionReply(int status, const QByteArray &raw)
{
    if (!m_chunkActive) {
        return;   // 取消/收尾后到达的过期响应，直接丢弃
    }
    if (status == 200) {
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        QJsonArray uploaded;
        if (doc.isObject()) {
            uploaded = doc.object().value(QStringLiteral("uploaded")).toArray();
        }
        beginUploading(uploaded);
    } else if (status == 404) {
        // upload_id 失效：清空本地已传集合，重新 init
        appendLog(QStringLiteral("续传"), buildUrl(QStringLiteral("/api/v1/uploads")), status, 0,
                  QStringLiteral("upload_id 失效（404），丢弃本地进度并重新 init"));
        m_doneSeq.clear();
        m_resumeHit = false;
        m_uploadId = 0;
        sendInit();
    } else {
        // 其他错误（含网络错误）：保守起见回到 init 重新建会话
        appendLog(QStringLiteral("续传"), buildUrl(QStringLiteral("/api/v1/uploads")), status, 0,
                  QStringLiteral("确认会话失败，改为重新 init"));
        m_doneSeq.clear();
        m_resumeHit = false;
        m_uploadId = 0;
        sendInit();
    }
}

void MainWindow::handleInitReply(int status, const QByteArray &raw)
{
    if (!m_chunkActive) {
        return;   // 取消/收尾后到达的过期 init 响应，直接丢弃（否则会重新拉起上传）
    }
    // 同名冲突（409）：询问用户；选择覆盖则带 overwrite:true 重发 init
    if (status == 409) {
        const QJsonObject o = QJsonDocument::fromJson(raw).object();
        const QString name = o.value(QStringLiteral("name")).toString(m_chunkName);
        const QString dir = o.value(QStringLiteral("dir")).toString(m_chunkDir);
        if (askOverwrite(dir, name)) {
            m_chunkOverwrite = true;
            sendInit();
            return;
        }
        finishChunkSession(false);   // 用户选择跳过
        return;
    }
    if (status != 200) {
        appendLog(QStringLiteral("init"), buildUrl(QStringLiteral("/api/v1/uploads/init")), status, 0,
                  QStringLiteral("init 失败：%1").arg(formatBody(raw)));
        if (status == 507 || status == 413) {
            QMessageBox::warning(
                this, QStringLiteral("服务器拒绝上传（%1）").arg(status),
                QStringLiteral("%1\n\n可用 GET /api/v1/storage 查看服务器剩余空间。")
                    .arg(formatBody(raw)));
        }
        finishChunkSession(false);
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (!doc.isObject()) {
        finishChunkSession(false);
        return;
    }
    const QJsonObject obj = doc.object();

    // 秒传命中：服务端已有相同内容，无需传分块
    // （契约字段为 done：server main.cpp init 秒传分支 v.set("done", true)；兼容旧名 instant）
    if (obj.value(QStringLiteral("done")).toBool()
        || obj.value(QStringLiteral("instant")).toBool()) {
        const QString fid = QString::number(obj.value(QStringLiteral("file_id")).toVariant().toLongLong());
        appendLog(QStringLiteral("秒传"), buildUrl(QStringLiteral("/api/v1/uploads/init")), 200, 0,
                  QStringLiteral("秒传命中，file_id=%1").arg(fid));
        clearManifest();
        finishChunkSession(true);
        return;
    }

    m_uploadId = obj.value(QStringLiteral("upload_id")).toVariant().toLongLong();
    // 分块总数以本地按 chunk_size 切出的分块表为准（服务端 init 不返回 total_chunks）
    m_totalChunks = m_chunks.size();

    beginUploading(obj.value(QStringLiteral("uploaded")).toArray());
}

void MainWindow::handleChunkReply(int status, const QString &errorString, const QByteArray &, int seq)
{
    if (!m_chunkActive || m_cancelRequested) {
        return;   // 会话已结束（含取消）或用户已取消：在途响应直接丢弃，不再写进度
    }
    m_inflightSeq.remove(seq);

    if (status != 200) {
        // 失败：指数退避重试（0.5 / 1 / 2 秒，最多 kMaxRetries 次）
        const int tries = m_chunkRetries.value(seq, 0);
        if (tries < kMaxRetries) {
            m_chunkRetries.insert(seq, tries + 1);
            const int delay = (tries == 0) ? 500 : (tries == 1 ? 1000 : 2000);
            appendLog(QStringLiteral("chunk"),
                      buildUrl(QStringLiteral("/api/v1/uploads/%1/chunk/%2").arg(m_uploadId).arg(seq)),
                      0, 0,
                      QStringLiteral("分块 seq=%1 失败（%2），第 %3 次重试，%4 ms 后继续")
                          .arg(seq)
                          .arg(status > 0 ? QStringLiteral("HTTP %1").arg(status) : errorString)
                          .arg(tries + 1).arg(delay));
            QTimer::singleShot(delay, this, [this, seq] {
                if (m_chunkActive && !m_cancelRequested) {
                    launchChunkAsync(seq);
                }
            });
            updateProgress();
            return;
        }
        appendLog(QStringLiteral("chunk"),
                  buildUrl(QStringLiteral("/api/v1/uploads/%1/chunk/%2").arg(m_uploadId).arg(seq)),
                  0, 0, QStringLiteral("分块 seq=%1 重试耗尽，上传失败").arg(seq));
        finishChunkSession(false);   // 保留 upload_id 与已传分块，供续传
        return;
    }

    // 成功：记下已传分块并立即落盘（每收到一个 ACK 都持久化）
    m_doneSeq.insert(seq);
    m_chunkRetries.remove(seq);
    saveManifest();
    updateProgress();
    pumpChunks();
}

void MainWindow::handleCompleteReply(int status, const QString &errorString, const QByteArray &raw)
{
    Q_UNUSED(errorString);
    if (!m_chunkActive || m_cancelRequested) {
        return;   // 会话已结束（含取消）或用户已取消：在途响应直接丢弃，不再写进度
    }
    if (status == 200) {
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (doc.isObject()) {
            const QJsonObject obj = doc.object();
            const QString fid = QString::number(obj.value(QStringLiteral("file_id")).toVariant().toLongLong());
            const QString name = obj.value(QStringLiteral("name")).toString();
            const qint64 size = obj.value(QStringLiteral("size")).toVariant().toLongLong();
            const QString hash = obj.value(QStringLiteral("hash")).toString();
            const qint64 createdAt = QDateTime::currentMSecsSinceEpoch();
            addRow(fid, name.isEmpty() ? m_chunkName : name, size, hash,
                   QStringLiteral("否"), createdAt);
            appendLog(QStringLiteral("complete"),
                      buildUrl(QStringLiteral("/api/v1/uploads/%1/complete").arg(m_uploadId)), 200, 0,
                      QStringLiteral("分块上传完成：file_id=%1，大小=%2，hash=%3")
                          .arg(fid).arg(formatSize(size)).arg(hash));
        }
        clearManifest();
        finishChunkSession(true);
        return;
    }

    if (status == 409) {
        // 缺分块：解析 missing，移除对应已传标记后补传
        QJsonArray missing;
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (doc.isObject()) {
            missing = doc.object().value(QStringLiteral("missing")).toArray();
        }
        QStringList missTxt;
        for (const QJsonValue &v : missing) {
            const int s = v.toInt();
            if (s >= 0 && s < m_totalChunks) {
                m_doneSeq.remove(s);
                m_chunkRetries.remove(s);
                missTxt.append(QString::number(s));
            }
        }
        appendLog(QStringLiteral("complete"),
                  buildUrl(QStringLiteral("/api/v1/uploads/%1/complete").arg(m_uploadId)), status, 0,
                  QStringLiteral("缺失分块 %1，正在补传").arg(missTxt.join(QLatin1Char(','))));
        m_completeSent = false;
        pumpChunks();
        return;
    }

    if (status == 422) {
        // 哈希不符：invalid 精确列出坏分块；为空表示各分块单检都通过、仅整文件哈希不符
        // （通常是本地声明的 hash 有误）——重传无济于事，直接失败并保留会话供排查
        QJsonArray invalid;
        QString reason;
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (doc.isObject()) {
            const QJsonObject obj = doc.object();
            invalid = obj.value(QStringLiteral("invalid")).toArray();
            reason = obj.value(QStringLiteral("reason")).toString();
        }
        if (invalid.isEmpty()) {
            appendLog(QStringLiteral("complete"),
                      buildUrl(QStringLiteral("/api/v1/uploads/%1/complete").arg(m_uploadId)),
                      status, 0,
                      QStringLiteral("整文件哈希不符且无坏分块（invalid 为空%1），终止重传，保留会话")
                          .arg(reason.isEmpty()
                                   ? QString()
                                   : QStringLiteral("：%1").arg(reason)));
            finishChunkSession(false);
            return;
        }
        QStringList badTxt;
        for (const QJsonValue &v : invalid) {
            const int s = v.toInt();
            if (s >= 0 && s < m_totalChunks) {
                m_doneSeq.remove(s);
                m_chunkRetries.remove(s);
                badTxt.append(QString::number(s));
            }
        }
        appendLog(QStringLiteral("complete"),
                  buildUrl(QStringLiteral("/api/v1/uploads/%1/complete").arg(m_uploadId)), status, 0,
                  QStringLiteral("坏分块 %1，正在重传").arg(badTxt.join(QLatin1Char(','))));
        m_completeSent = false;
        pumpChunks();
        return;
    }

    // 其他错误：保留进度供续传
    appendLog(QStringLiteral("complete"),
              buildUrl(QStringLiteral("/api/v1/uploads/%1/complete").arg(m_uploadId)), status, 0,
              QStringLiteral("合并失败：%1").arg(formatBody(raw)));
    finishChunkSession(false);
}


int MainWindow::uploadedCount() const
{
    int n = 0;
    for (int seq = 0; seq < m_totalChunks; ++seq) {
        if (m_doneSeq.contains(seq)) {
            ++n;
        }
    }
    return n;
}

void MainWindow::updateProgress()
{
    const int total = m_totalChunks;
    const int done = m_chunkActive ? uploadedCount() : 0;
    m_progressBar->setRange(0, total > 0 ? total : 1);
    m_progressBar->setValue(total > 0 ? qMin(done, total) : 0);
    if (m_chunkActive) {
        const int pct = total > 0 ? static_cast<int>(done * 100LL / total) : 0;
        m_progressLabel->setText(
            QStringLiteral("分块上传进度：%1 / %2 块（%3%%4）  upload_id=%5%6")
                .arg(done).arg(total).arg(pct).arg(QLatin1Char('%')).arg(m_uploadId)
                .arg(m_resumeHit ? QStringLiteral("（命中续传）") : QString()));
    } else {
        // 分块下载时进度文案由 handleDownloadChunkReply 直接更新 m_progressLabel
        m_progressLabel->setText(m_dlActive ? m_progressLabel->text()
                                            : QStringLiteral("进度：空闲"));
    }
}

void MainWindow::updateCancelButton()
{
    m_cancelUploadBtn->setEnabled(m_chunkActive);
}

void MainWindow::finishChunkSession(bool ok)
{
    const bool wasCancelled = m_cancelRequested;   // 先留存：下方会复位该标志
    m_chunkActive = false;
    m_inflightSeq.clear();
    m_cancelRequested = false;
    updateCancelButton();
    updateProgress();

    appendLog(QStringLiteral("结束"), buildUrl(QStringLiteral("/api/v1/uploads")), 0, 0,
              ok ? QStringLiteral("分块上传成功，会话已清理")
                 : QStringLiteral("分块上传中断，保留 upload_id=%1（重选同一文件可从断点续传）")
                       .arg(m_uploadId));

    if (ok) {
        // 成功：清空会话状态
        m_uploadId = 0;
        m_doneSeq.clear();
        m_chunks.clear();
        m_chunkPath.clear();
        m_resumeHit = false;
        m_completeSent = false;
        m_chunkRetries.clear();
        m_totalChunks = 0;
    }
    // 失败（ok=false）：保留 m_uploadId / m_chunkPath / m_doneSeq，供下次续传

    // 批量上传：一个文件（分块大文件）结束后推进队列
    if (m_upBatch) {
        if (ok) {
            ++m_upOk;
        } else {
            ++m_upFail;
        }
        if (wasCancelled) {
            m_upQueue.clear();   // 用户取消 → 终止整批
        }
        QTimer::singleShot(0, this, [this] { startNextUpload(); });
    }
}

// ---------------------------------------------------------------------------
//  分块下载（断点续传）
// ---------------------------------------------------------------------------
// 拉取 [offset, offset+kDownloadChunkSize) 这一段（有界 Range）。
// 有界是关键：只发 `bytes=start-` 会让服务端把从 start 到文件尾全部回发，大文件同样会撑爆内存。
// 空文件（total==0）不发 Range，让服务端走 200 空响应。
void MainWindow::sendDownloadRange(qint64 offset)
{
    QNetworkRequest req(buildUrl(QStringLiteral("/api/v1/files/%1/content").arg(m_dlId)));
    if (m_dlTotal != 0) {
        const qint64 end = offset + kDownloadChunkSize - 1;
        req.setRawHeader("Range", QStringLiteral("bytes=%1-%2").arg(offset).arg(end).toUtf8());
    }
    QNetworkReply *r = sendRequest(req, "GET");
    r->setProperty("cvStep", QStringLiteral("dlchunk"));
    r->setProperty("cvOffset", offset);
}

void MainWindow::handleDownloadChunkReply(int status, bool networkError, const QString &errorString,
                                          const QByteArray &raw, qint64 dlTotal, qint64 dlStart)
{
    if (!m_dlActive) {
        return;   // 已被取消 / 结束
    }
    if (networkError || (status != 200 && status != 206)) {
        appendLog(QStringLiteral("下载"),
                  buildUrl(QStringLiteral("/api/v1/files/%1/content").arg(m_dlId)), status, 0,
                  QStringLiteral("[下载失败] %1（HTTP %2）")
                      .arg(status > 0 ? formatBody(raw) : errorString).arg(status));
        finishDownload(false);
        return;
    }

    if (status == 200) {
        // 服务端不支持 Range，返回整文件：直接写最终路径
        QFile out(m_dlFinalPath);
        if (!out.open(QIODevice::WriteOnly)) {
            appendLog(QStringLiteral("下载"), QUrl::fromLocalFile(m_dlFinalPath), 0, 0,
                      QStringLiteral("[落盘失败] %1").arg(out.errorString()));
            finishDownload(false);
            return;
        }
        out.write(raw);
        out.close();
        QFile::remove(m_dlPartPath);
        appendLog(QStringLiteral("下载"), QUrl::fromLocalFile(m_dlFinalPath), 200, 0,
                  QStringLiteral("服务端不支持 Range，整文件写入 %1（%2）")
                      .arg(m_dlFinalPath).arg(formatSize(raw.size())));
        finishDownload(true);
        return;
    }

    // 206：先校验服务端返回起点与本地断点 offset 一致，再追加写入 .part
    if (dlStart >= 0 && dlStart != m_dlOffset) {
        appendLog(QStringLiteral("下载"),
                  buildUrl(QStringLiteral("/api/v1/files/%1/content").arg(m_dlId)), status, 0,
                  QStringLiteral("[下载失败] 服务端返回起点 %1 与本地断点 %2 不一致，"
                                 "终止以免写坏 .part")
                      .arg(dlStart).arg(m_dlOffset));
        finishDownload(false);
        return;
    }
    if (dlTotal > 0) {
        m_dlTotal = dlTotal;   // 以 Content-Range 的 total 为准
    }
    // 落盘放到线程池（每段 4MiB 写盘不该阻塞 UI）；写完回主线程推进进度/下一段
    appendDownloadSegmentAsync(raw, m_dlOffset);
}

// 后台追加写下载段（先 seek 到 offset 再写，支持断点续传语义）
void MainWindow::appendDownloadSegmentAsync(const QByteArray &data, qint64 offset)
{
    const QString partPath = m_dlPartPath;
    QThreadPool *pool = m_ioPool;
    QPointer<MainWindow> self(this);
    pool->start( [self, partPath, data, offset]() {
        bool ok = false;
        QString err;
        QFile part(partPath);
        if (!part.open(QIODevice::ReadWrite)) {
            err = part.errorString();
        } else if (!part.seek(offset)) {
            err = QStringLiteral("seek 失败：%1").arg(part.errorString());
            part.close();
        } else if (part.write(data) != data.size()) {
            err = QStringLiteral("写入不足：%1").arg(part.errorString());
            part.close();
        } else {
            part.close();
            ok = true;
        }
        if (self) {
            QMetaObject::invokeMethod(
                self,
                [self, ok, err]() { if (self) self->onDownloadSegmentWritten(ok, err, 0); },
                Qt::QueuedConnection);
        }
    });
}

void MainWindow::onDownloadSegmentWritten(bool ok, const QString &err, qint64 bytes)
{
    Q_UNUSED(bytes);
    if (!m_dlActive) {
        return;   // 已被取消/结束
    }
    if (!ok) {
        appendLog(QStringLiteral("下载"), QUrl::fromLocalFile(m_dlPartPath), 0, 0,
                  QStringLiteral("[落盘失败] %1").arg(err));
        finishDownload(false);
        return;
    }
    // 实际落盘字节数 = .part 当前大小（seek+write 后由文件系统裁定）
    m_dlOffset = QFileInfo(m_dlPartPath).size();

    const int pct = m_dlTotal > 0 ? static_cast<int>(m_dlOffset * 100LL / m_dlTotal) : 0;
    // 进度条用 0..1000 千分比：直接 setRange(0, m_dlTotal) 在 >2GiB 时会 int 溢出
    m_progressBar->setRange(0, 1000);
    m_progressBar->setValue(pct * 10);
    m_progressLabel->setText(QStringLiteral("分块下载进度：%1 / %2（%3%%4）")
                                 .arg(formatSize(m_dlOffset)).arg(formatSize(m_dlTotal))
                                 .arg(pct).arg(QLatin1Char('%')));

    if (m_dlTotal > 0 && m_dlOffset >= m_dlTotal) {
        finalizeDownload();
    } else {
        sendDownloadRange(m_dlOffset);   // 继续下一段
    }
}

// 校验大小，把 .part 重命名为正式文件
void MainWindow::finalizeDownload()
{
    QFileInfo fi(m_dlPartPath);
    if (m_dlTotal > 0 && fi.size() != m_dlTotal) {
        appendLog(QStringLiteral("下载"), QUrl::fromLocalFile(m_dlPartPath), 0, 0,
                  QStringLiteral("[大小不符] .part=%1，期望=%2").arg(formatSize(fi.size()))
                      .arg(formatSize(m_dlTotal)));
    }
    if (QFile::exists(m_dlFinalPath)) {
        QFile::remove(m_dlFinalPath);
    }
    QFile::rename(m_dlPartPath, m_dlFinalPath);
    appendLog(QStringLiteral("下载"), QUrl::fromLocalFile(m_dlFinalPath), 200, 0,
              QStringLiteral("已保存 id=%1 到 %2（%3）")
                  .arg(m_dlId, m_dlFinalPath, formatSize(m_dlOffset)));
    finishDownload(true);
}

void MainWindow::finishDownload(bool ok)
{
    m_dlActive = false;
    updateProgress();

    if (m_dlBatch) {
        if (ok) {
            ++m_dlOk;
        } else {
            ++m_dlFail;
        }
        showStatus(QStringLiteral("… 下载中 %1/%2")
                       .arg(m_dlOk + m_dlFail)
                       .arg(m_dlOk + m_dlFail + m_dlQueue.size()),
                   true);
        QTimer::singleShot(0, this, [this] { startNextDownload(); });
        return;
    }
    showStatus(ok ? QStringLiteral("✓ 下载完成") : QStringLiteral("✗ 下载失败"), ok);
}

// ---------------------------------------------------------------------------
//  路径校验（供文件树对话框"新建文件夹"复用）
// ---------------------------------------------------------------------------

// 客户端侧路径预检：与服务端 sanitizeRelPath 同规则。
// 这里只做快速反馈，最终裁决始终在服务端（双保险，前端规则不可作为安全边界）。
bool MainWindow::validRelPathInput(const QString &in, QString *why)
{
    const auto fail = [why](const QString &m) {
        if (why) *why = m;
        return false;
    };
    if (in.isEmpty()) {
        return fail(QStringLiteral("路径为空"));
    }
    if (in.size() > 400) {
        return fail(QStringLiteral("路径过长（>400 字符）"));
    }
    if (in.startsWith(QLatin1Char('/')) || in.startsWith(QLatin1Char('\\'))) {
        return fail(QStringLiteral("不允许绝对路径（不得以 / 或 \\ 开头）"));
    }
    if (in.contains(QLatin1Char('\\'))) {
        return fail(QStringLiteral("不允许反斜杠，请使用 '/' 作为分隔符"));
    }
    if (in.contains(QLatin1Char(':'))) {
        return fail(QStringLiteral("不允许冒号（盘符 / 保留字符）"));
    }
    for (const QChar ch : in) {
        const ushort u = ch.unicode();
        if (u < 0x20 || u == 0x7F || ch == QLatin1Char('<') || ch == QLatin1Char('>')
            || ch == QLatin1Char('"') || ch == QLatin1Char('|') || ch == QLatin1Char('?')
            || ch == QLatin1Char('*')) {
            return fail(QStringLiteral("包含非法字符（< > \" | ? * 或控制字符）"));
        }
    }
    const QStringList segs = in.split(QLatin1Char('/'));
    for (const QString &seg : segs) {
        if (seg.isEmpty()) {
            return fail(QStringLiteral("存在空路径段（如 a//b 或结尾 '/'）"));
        }
        if (seg == QStringLiteral(".") || seg == QStringLiteral("..")) {
            return fail(QStringLiteral("不允许 '.' / '..' 路径段（防止目录穿越）"));
        }
        if (seg.endsWith(QLatin1Char('.')) || seg.endsWith(QLatin1Char(' '))) {
            return fail(QStringLiteral("路径段不能以 '.' 或空格结尾：%1").arg(seg));
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
//  网络
// ---------------------------------------------------------------------------
QUrl MainWindow::buildUrl(const QString &path) const
{
    QString base = m_serverEdit->text().trimmed();
    while (base.endsWith(QLatin1Char('/'))) {
        base.chop(1);
    }
    if (base.isEmpty()) {
        base = defaultServerUrl();
    }
    // 容忍只填 IP 或 IP:端口 —— 缺 scheme 时补 http://
    // （旧实现在缺 scheme 时静默退回默认地址，导致用户填了服务器 IP 也连不上）
    if (!base.startsWith(QStringLiteral("http://"))
        && !base.startsWith(QStringLiteral("https://"))) {
        base.prepend(QStringLiteral("http://"));
    }
    QUrl url(base + path);
    if (url.port() < 0) {
        url.setPort(kDefaultPort);   // 未显式带端口时补默认端口
    }
    return url;
}

QNetworkReply *MainWindow::sendRequest(const QNetworkRequest &request, const QByteArray &verb,
                                       const QByteArray &body)
{
    QNetworkReply *reply = nullptr;
    if (verb == "GET") {
        reply = m_nam->get(request);
    } else if (verb == "PUT") {
        reply = m_nam->put(request, body);
    } else if (verb == "DELETE") {
        reply = m_nam->deleteResource(request);
    } else {
        reply = m_nam->post(request, body);
    }
    // 记下实际动词，供日志显示（原有的 operation() 只能区分 GET / 非 GET）
    reply->setProperty("cvVerb", QString::fromUtf8(verb));

    QElapsedTimer timer;
    timer.start();
    m_timers.insert(reply, timer);

    ++m_pending;
    setBusy(true);
    return reply;
}

void MainWindow::onReplyFinished(QNetworkReply *reply)
{
    // 响应体只能读一次，所以在这里统一取出，再交给 handleReply 分派
    const QByteArray raw = reply->readAll();
    handleReply(reply, raw);
    reply->deleteLater();

    if (--m_pending <= 0) {
        m_pending = 0;
        setBusy(false);
    }
}

void MainWindow::handleReply(QNetworkReply *reply, const QByteArray &raw)
{
    const qint64 elapsed = m_timers.contains(reply) ? m_timers.take(reply).elapsed() : -1;
    const QUrl url = reply->request().url();
    // 实际动词优先取 cvVerb（GET/PUT/DELETE/POST），否则按 operation() 兜底
    const QString method = reply->property("cvVerb").isValid()
        ? reply->property("cvVerb").toString()
        : ((reply->operation() == QNetworkAccessManager::GetOperation) ? QStringLiteral("GET")
                                                                      : QStringLiteral("POST"));
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    // 分块上传 / 分块下载的请求统一走这里收口（它们各自带 cvStep 标记）
    const QString step = reply->property("cvStep").toString();
    if (!step.isEmpty()) {
        const bool canceled = (reply->error() == QNetworkReply::OperationCanceledError);
        if (!canceled) {
            appendLog(method, url, status, elapsed,
                      (reply->error() != QNetworkReply::NoError)
                          ? QStringLiteral("[网络错误] %1").arg(reply->errorString())
                          : formatBody(raw));
        }
        if (canceled) {
            return;
        }
        // 下载响应需要从 Content-Range 解析起点与 total（形如 "bytes 123-456/789"）
        qint64 dlTotal = 0;
        qint64 dlStart = -1;   // -1 = 无 Content-Range（200 整文件响应）
        const QByteArray crh = reply->rawHeader("Content-Range");
        if (!crh.isEmpty()) {
            QByteArray spec = crh;
            if (spec.startsWith("bytes ")) {
                spec = spec.mid(6);
            }
            const int dash = spec.indexOf('-');
            const int slash = spec.lastIndexOf('/');
            if (dash > 0) {
                dlStart = spec.left(dash).trimmed().toLongLong();
            }
            if (slash > dash) {
                dlTotal = spec.mid(slash + 1).toLongLong();
            }
        }
        const int seq = reply->property("cvSeq").toInt();
        handleChunkedReply(step, seq, status, reply->error() != QNetworkReply::NoError,
                           reply->errorString(), raw, dlTotal, dlStart);
        return;
    }

    // 预览请求单独收口：它与下载同 URL，靠动态属性区分，避免被当成下载落盘
    if (reply->property("cvPreview").toBool()) {
        const bool isCancel = (reply->error() == QNetworkReply::OperationCanceledError);
        if (!isCancel) {
            appendLog(method, url, status, elapsed,
                      (reply->error() != QNetworkReply::NoError)
                          ? QStringLiteral("[网络错误] %1").arg(reply->errorString())
                          : formatBody(raw));
        }
        if (m_previewReply == reply) {
            m_previewReply = nullptr;
        }
        // 已取消的请求不碰界面，界面已经被后一次选中接管
        if (isCancel) {
            return;
        }
        handlePreviewReply(status, reply->errorString(), reply->error() != QNetworkReply::NoError,
                           raw);
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        appendLog(method, url, status, elapsed,
                  QStringLiteral("✗ 网络错误：%1").arg(reply->errorString()));
        // 下载的网络错误由 handleDownloadChunkReply 统一收尾（它带 cvStep="dlchunk"，
        // 在更早的步骤分派里已经处理），这里只需推进批量上传队列
        if (m_upBatch) {
            ++m_upFail;
            QTimer::singleShot(0, this, [this] { startNextUpload(); });
        }
        return;
    }

    appendLog(method, url, status, elapsed, formatBody(raw));

    // 按路径把响应分派给对应的处理逻辑（下载走 cvStep=dlchunk，在更早处已分派）
    const QString path = url.path();
    if (path.endsWith(QStringLiteral("/api/v1/files")) && method == QStringLiteral("POST")) {
        handleUploadReply(status, raw);
    } else if (path.endsWith(QStringLiteral("/api/v1/files"))) {
        handleListReply(raw);
    }
    // /healthz 与 /api/v1/files/:id 只需要看日志，不做额外处理
}

void MainWindow::handleUploadReply(int status, const QByteArray &raw)
{
    const bool okStatus = (status == 200 || status == 201);

    // 同名冲突（409）→ 询问用户；选择覆盖则带 X-CV-Overwrite 重发同一个文件，不推进队列
    if (status == 409) {
        const QJsonObject o = QJsonDocument::fromJson(raw).object();
        const QString name = o.value(QStringLiteral("name"))
                                 .toString(QFileInfo(m_upCurrentPath).fileName());
        const QString dir = o.value(QStringLiteral("dir")).toString(m_upDir);
        if (askOverwrite(dir, name)) {
            sendWholeFile(m_upCurrentPath, true);
            return;
        }
        ++m_upFail;   // 用户选择跳过
        QTimer::singleShot(0, this, [this] { startNextUpload(); });
        return;
    }

    // 服务端空间不足（507）/ 超出单次上限（413）：明确告警（批量时只提示一次）
    if (!okStatus && (status == 507 || status == 413) && !m_upSpaceWarned) {
        m_upSpaceWarned = true;
        QMessageBox::warning(
            this, QStringLiteral("服务器拒绝上传（%1）").arg(status),
            QStringLiteral("%1\n\n可用 GET /api/v1/storage 查看服务器剩余空间。")
                .arg(formatBody(raw)));
    }

    if (okStatus) {
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (doc.isObject()) {
            const QJsonObject obj = doc.object();

            const QString id =
                QString::number(obj.value(QStringLiteral("id")).toVariant().toLongLong());
            const bool instant = obj.value(QStringLiteral("instant")).toBool();
            m_instantById.insert(id, instant);

            // 上传响应按契约不带 created_at，用本地当前时间兜底（文件确实是刚刚传上去的）；
            // 保持与服务端的毫秒单位一致，显示时由 formatTime 归一化
            const qint64 createdAt = obj.contains(QStringLiteral("created_at"))
                ? obj.value(QStringLiteral("created_at")).toVariant().toLongLong()
                : QDateTime::currentMSecsSinceEpoch();

            // 上传成功后直接把这条补进列表，省得用户再点一次【列出文件】
            addRow(id, obj.value(QStringLiteral("name")).toString(),
                   obj.value(QStringLiteral("size")).toVariant().toLongLong(),
                   obj.value(QStringLiteral("hash")).toString(),
                   instant ? QStringLiteral("是") : QStringLiteral("否"), createdAt, false,
                   obj.value(QStringLiteral("dir")).toString());
        }
    }

    if (m_upBatch) {
        if (okStatus) {
            ++m_upOk;
        } else {
            ++m_upFail;   // 非 2xx 必须计失败（此前一律计成功，是 bug）
        }
        showStatus(QStringLiteral("… 上传中 %1/%2")
                       .arg(m_upOk + m_upFail)
                       .arg(m_upOk + m_upFail + m_upQueue.size()),
                   true);
        startNextUpload();
    }
}

void MainWindow::handleListReply(const QByteArray &raw)
{
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (!doc.isObject()) {
        return;
    }
    const QJsonArray items = doc.object().value(QStringLiteral("items")).toArray();

    // 全量刷新：重建数据模型后统一渲染（屏蔽信号避免每行触发预览请求）
    m_rows.clear();
    for (const QJsonValue &v : items) {
        if (!v.isObject()) {
            continue;
        }
        const QJsonObject o = v.toObject();
        RowData r;
        r.id = QString::number(o.value(QStringLiteral("id")).toVariant().toLongLong());
        r.name = o.value(QStringLiteral("name")).toString();
        r.dir = o.value(QStringLiteral("dir")).toString();
        r.size = o.value(QStringLiteral("size")).toVariant().toLongLong();
        r.createdAt = o.value(QStringLiteral("created_at")).toVariant().toLongLong();
        r.hash = o.value(QStringLiteral("hash")).toString();
        r.instantText = QStringLiteral("-");   // 列表接口没有 instant 字段
        if (m_instantById.contains(r.id)) {
            r.instantText = m_instantById.value(r.id) ? QStringLiteral("是") : QStringLiteral("否");
        }
        m_rows.append(r);
    }
    applySort(m_sortKey, m_sortAsc);   // 重建后保持当前排序

    // 刷新后没有选中行：同步清掉预览区，避免它还停留在刷新前那个文件上
    if (m_table->currentRow() < 0) {
        cancelPreview();
        m_previewId.clear();
        resetPreview();
    }
}

// 查询服务器剩余空间并更新顶部标签（GET /api/v1/storage）
void MainWindow::refreshStorage()
{
    QNetworkRequest req(buildUrl(QStringLiteral("/api/v1/storage")));
    QNetworkReply *r = sendRequest(req, "GET");
    r->setProperty("cvStep", QStringLiteral("storage"));
}

void MainWindow::applyStorageInfo(const QByteArray &raw)
{
    const QJsonObject o = QJsonDocument::fromJson(raw).object();
    const qint64 freeB = o.value(QStringLiteral("free_bytes")).toVariant().toLongLong();
    const qint64 totalB = o.value(QStringLiteral("total_bytes")).toVariant().toLongLong();
    if (freeB < 0) {
        m_spaceLabel->setText(QStringLiteral("服务器空间：不可用"));
        return;
    }
    const int pct = totalB > 0 ? static_cast<int>(freeB * 100LL / totalB) : 0;
    m_spaceLabel->setText(QStringLiteral("服务器剩余：%1 / %2（%3%%4）")
                              .arg(formatSize(freeB), formatSize(totalB))
                              .arg(pct).arg(QLatin1Char('%')));
    // 低于 10%（或不足 512MiB）标红提示
    const bool low = (totalB > 0 && pct < 10) || (freeB < 512LL * 1024 * 1024);
    m_spaceLabel->setStyleSheet(low ? QStringLiteral("color:#cf222e; font-weight:bold;")
                                    : QStringLiteral("color:#57606a;"));
}

// 顶部结果标识：✓ 绿色 / ✗ 红色（简化反馈，不用看日志也能判断成败）
void MainWindow::showStatus(const QString &text, bool ok)
{
    if (!m_statusLabel) {
        return;
    }
    m_statusLabel->setText(text);
    m_statusLabel->setStyleSheet(ok ? QStringLiteral("color:#1a7f37; font-weight:bold;")
                                    : QStringLiteral("color:#cf222e; font-weight:bold;"));
}

// ---------------------------------------------------------------------------
//  状态与展示
// ---------------------------------------------------------------------------
void MainWindow::setBusy(bool busy)
{
    m_healthBtn->setEnabled(!busy);
    m_uploadBtn->setEnabled(!busy);
    m_listBtn->setEnabled(!busy);
    m_downloadBtn->setEnabled(!busy);
    m_chunkUploadBtn->setEnabled(!busy);
    m_dlResumeBtn->setEnabled(!busy);
    // 分块上传/批量传输会持续产生大量短请求，若每个都切忙碌光标会不停闪烁；
    // 这类场景只禁用按钮，不改光标（IO 已在线程池，界面保持可交互）
    if (busy && !m_chunkActive && !m_upBatch && !m_dlBatch) {
        setCursor(Qt::BusyCursor);
    } else {
        unsetCursor();
    }
    updateCancelButton();   // 会话中保持【取消上传】可用
}

void MainWindow::appendLog(const QString &method, const QUrl &url, int status, qint64 elapsedMs,
                           const QString &body)
{
    const QString head = (status > 0)
        ? QStringLiteral("%1 %2\n  -> HTTP %3  耗时 %4 ms")
              .arg(method, url.toString(), QString::number(status), QString::number(elapsedMs))
        : QStringLiteral("%1 %2").arg(method, url.toString());

    m_log->appendPlainText(head + (body.isEmpty() ? QString() : QStringLiteral("\n%1").arg(body)));
    m_log->appendPlainText(QStringLiteral(""));
    m_log->ensureCursorVisible();
}

// doSelect=false（默认）：插完行不选中，避免刷新列表 / 上传完成时顺带触发一次预览请求
void MainWindow::addRow(const QString &id, const QString &name, qint64 size, const QString &hash,
                        const QString &instantText, qint64 createdAt, bool doSelect,
                        const QString &dir)
{
    RowData r;
    r.id = id;
    r.name = name;
    r.size = size;
    r.hash = hash;
    r.instantText = instantText;
    r.createdAt = createdAt;
    r.dir = dir;
    m_rows.append(r);
    renderRows();
    if (doSelect) {
        applySort(m_sortKey, m_sortAsc);   // 保持当前排序后，选中新插入那行的 id
        for (int row = 0; row < m_table->rowCount(); ++row) {
            if (m_table->item(row, 0) && m_table->item(row, 0)->data(Qt::UserRole).toString() == id) {
                const bool wasBlocked = m_table->signalsBlocked();
                m_table->blockSignals(true);
                m_table->selectRow(row);
                m_table->blockSignals(wasBlocked);
                break;
            }
        }
    }
}

// 按 m_rows 重建表格（排序只改 m_rows，渲染始终走这里）
void MainWindow::renderRows()
{
    const bool wasBlocked = m_table->signalsBlocked();
    m_table->blockSignals(true);
    m_table->setRowCount(0);
    for (const RowData &r : m_rows) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        auto setCell = [this, row](int col, const QString &text) {
            QTableWidgetItem *item = new QTableWidgetItem(text);
            item->setToolTip(text);
            m_table->setItem(row, col, item);
            return item;
        };
        setCell(0, r.id)->setData(Qt::UserRole, r.id);
        QTableWidgetItem *nameItem = setCell(1, r.name);
        nameItem->setData(Qt::UserRole + 1, r.dir);   // 右键菜单"上传到此目录"用
        setCell(2, formatSize(r.size))->setData(Qt::UserRole, r.size);
        setCell(3, formatTime(r.createdAt))->setData(Qt::UserRole, r.createdAt);
        setCell(4, r.hash.left(8));
        setCell(5, r.instantText);
    }
    m_table->blockSignals(wasBlocked);
}

// 排序：key 0=名称 1=大小 2=时间；只重排 m_rows 再渲染
void MainWindow::applySort(int key, bool asc)
{
    m_sortKey = key;
    m_sortAsc = asc;
    std::stable_sort(m_rows.begin(), m_rows.end(), [key, asc](const RowData &a, const RowData &b) {
        int cmp = 0;
        if (key == 1) {
            cmp = (a.size < b.size) ? -1 : (a.size > b.size ? 1 : 0);
        } else if (key == 2) {
            cmp = (a.createdAt < b.createdAt) ? -1 : (a.createdAt > b.createdAt ? 1 : 0);
        } else {
            cmp = QString::compare(a.name, b.name, Qt::CaseInsensitive);
            if (cmp == 0) {
                cmp = QString::compare(a.dir, b.dir, Qt::CaseInsensitive);
            }
        }
        return asc ? (cmp < 0) : (cmp > 0);
    });
    renderRows();
}

QString MainWindow::currentFileId() const
{
    const int row = m_table->currentRow();
    if (row < 0) {
        return QString();
    }
    QTableWidgetItem *item = m_table->item(row, 0);
    return item ? item->data(Qt::UserRole).toString() : QString();
}

QString MainWindow::currentFileName() const
{
    const int row = m_table->currentRow();
    if (row < 0) {
        return QString();
    }
    QTableWidgetItem *item = m_table->item(row, 1);
    return item ? item->text() : QString();
}

qint64 MainWindow::currentFileSize() const
{
    const int row = m_table->currentRow();
    if (row < 0) {
        return -1;
    }
    QTableWidgetItem *item = m_table->item(row, 2);
    return item ? item->data(Qt::UserRole).toLongLong() : -1;
}

qint64 MainWindow::currentFileCreatedAt() const
{
    const int row = m_table->currentRow();
    if (row < 0) {
        return 0;
    }
    QTableWidgetItem *item = m_table->item(row, 3);
    return item ? item->data(Qt::UserRole).toLongLong() : 0;
}

QString MainWindow::formatTime(qint64 epoch)
{
    // 0 / 缺失 / 非法都显示 "-"，避免误导成 1970-01-01
    if (epoch <= 0) {
        return QStringLiteral("-");
    }
    // 服务端 created_at 写的是 Unix 纪元毫秒，这里统一归一化成秒：超过 1e11 即可认定是毫秒量级
    if (epoch > 100000000000LL) {
        epoch /= 1000;
    }
    const QDateTime dt = QDateTime::fromSecsSinceEpoch(epoch).toLocalTime();
    if (!dt.isValid()) {
        return QStringLiteral("-");
    }
    return dt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

bool MainWindow::looksBinary(const QByteArray &data)
{
    const qsizetype n = data.size() < kBinaryScanBytes ? data.size() : kBinaryScanBytes;
    for (qsizetype i = 0; i < n; ++i) {
        if (static_cast<unsigned char>(data.at(i)) == 0) {
            return true;   // 文本里不该出现 NUL
        }
    }
    // 含非法 UTF-8 序列也按二进制处理（QString::fromUtf8 会把它们替换成 U+FFFD，
    // 回写后与原文不一致，据此判定）
    return QString::fromUtf8(data).toUtf8() != data;
}

QString MainWindow::toHexDump(const QByteArray &data)
{
    QString out;
    out.reserve(data.size() * 4 + 128);
    for (qsizetype offset = 0; offset < data.size(); offset += 16) {
        const QByteArray line = data.mid(offset, 16);
        QString hex;
        QString ascii;
        for (qsizetype i = 0; i < 16; ++i) {
            if (i == 8) {
                hex += QLatin1Char(' ');   // 中间加个空格，便于数字节
            }
            if (i < line.size()) {
                const unsigned char c = static_cast<unsigned char>(line.at(i));
                hex += QStringLiteral("%1 ").arg(static_cast<qulonglong>(c), 2, 16,
                                                 QLatin1Char('0'));
                ascii += (c >= 0x20 && c < 0x7F) ? QLatin1Char(static_cast<char>(c))
                                                 : QLatin1Char('.');
            } else {
                hex += QStringLiteral("   ");
            }
        }
        out += QStringLiteral("%1  ").arg(static_cast<qlonglong>(offset), 8, 16,
                                          QLatin1Char('0'));
        out += hex + QStringLiteral(" |") + ascii + QStringLiteral("|\n");
    }
    return out;
}

QString MainWindow::formatSize(qint64 bytes)
{
    if (bytes < 1024) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    double value = static_cast<double>(bytes);
    const char *units[] = {"KB", "MB", "GB", "TB"};
    int unit = -1;
    do {
        value /= 1024.0;
        ++unit;
    } while (value >= 1024.0 && unit < 3);
    return QStringLiteral("%1 %2").arg(QString::number(value, 'f', 2),
                                       QString::fromUtf8(units[unit]));
}

QString MainWindow::formatBody(const QByteArray &raw)
{
    if (raw.isEmpty()) {
        return QString();
    }

    // 二进制响应体（下载内容、分块回读等）绝不按文本转码，否则会整段乱码。
    // 只给「大小 + 前 N 字节十六进制摘要」——文件过大时"显示部分即可"。
    if (looksBinary(raw)) {
        const qsizetype preview =
            raw.size() < kBodyBinaryPreviewBytes ? raw.size() : kBodyBinaryPreviewBytes;
        return QStringLiteral("（二进制响应体 %1，前 %2 字节：%3%4）")
            .arg(formatSize(raw.size()))
            .arg(preview)
            .arg(QString::fromLatin1(raw.left(preview).toHex(' ')))
            .arg(raw.size() > preview ? QStringLiteral(" …") : QString());
    }

    // 先尝试按 JSON 美化输出，失败则按纯文本展示
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
    QString text;
    if (err.error == QJsonParseError::NoError) {
        text = QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
    } else {
        text = QString::fromUtf8(raw);
    }

    // 过长文本只展示前 kMaxBodyInLog 个字符（且在字符边界截断，不产生乱码）
    if (text.size() > kMaxBodyInLog) {
        QString head = text.left(kMaxBodyInLog);
        if (!head.isEmpty() && head.at(head.size() - 1).isHighSurrogate()) {
            head.chop(1);   // 避免切断代理对
        }
        return QStringLiteral("%1\n…（响应共 %2，已省略后 %3 字符）")
            .arg(head, formatSize(raw.size()), QString::number(text.size() - head.size()));
    }
    return text;
}
