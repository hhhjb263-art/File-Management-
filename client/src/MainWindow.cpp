// 云匣 CloudVault 测试客户端 —— 主窗口实现
//
// 与服务端（server/src/app/main.cpp）约定的接口：
//   GET  /healthz                    健康检查          -> {"status":"ok","version":...,"data_dir":...}
//   POST /api/v1/files               上传（body 原样） -> 201 {"id","name","size","hash","chunks","instant"}
//                                    文件名走请求头 X-CV-Name（百分号编码）
//   GET  /api/v1/files               列表              -> {"total":N,"items":[{id,name,size,hash,chunks,created_at}]}
//   GET  /api/v1/files/:id           元数据            -> {id,name,size,hash,chunks,created_at}
//   GET  /api/v1/files/:id/content   下载              -> application/octet-stream
//
// 说明：列表接口不返回 instant 字段，因此"是否秒传"一列取自本地记录的上传结果，
//       未经由本客户端上传过的文件显示为 "-"。

#include "MainWindow.h"

#include <QByteArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
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

// 日志里单条响应最多展示的字节数，避免 6MB 大文件把日志区刷爆
constexpr int kMaxBodyInLog = 4096;

// 服务端默认端口
constexpr int kDefaultPort = 8080;

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
    return QStringLiteral("http://127.0.0.1:%1").arg(kDefaultPort);
}

}  // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("云匣 CloudVault 测试客户端"));
    resize(900, 620);

    m_nam = new QNetworkAccessManager(this);
    connect(m_nam, &QNetworkAccessManager::finished, this, &MainWindow::onReplyFinished);

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
}

// ---------------------------------------------------------------------------
//  界面搭建
// ---------------------------------------------------------------------------
QWidget *MainWindow::createTopBar()
{
    QWidget *bar = new QWidget(this);

    QLabel *label = new QLabel(QStringLiteral("服务器地址："), bar);
    m_serverEdit = new QLineEdit(defaultServerUrl(), bar);
    m_serverEdit->setClearButtonEnabled(true);
    m_serverEdit->setMinimumWidth(260);

    m_healthBtn = new QPushButton(QStringLiteral("健康检查"), bar);
    m_uploadBtn = new QPushButton(QStringLiteral("选择文件并上传"), bar);
    m_listBtn = new QPushButton(QStringLiteral("列出文件"), bar);
    m_downloadBtn = new QPushButton(QStringLiteral("下载选中文件"), bar);
    m_clearLogBtn = new QPushButton(QStringLiteral("清空日志"), bar);

    connect(m_healthBtn, &QPushButton::clicked, this, &MainWindow::onHealthCheck);
    connect(m_uploadBtn, &QPushButton::clicked, this, &MainWindow::onUpload);
    connect(m_listBtn, &QPushButton::clicked, this, &MainWindow::onListFiles);
    connect(m_downloadBtn, &QPushButton::clicked, this, &MainWindow::onDownload);
    connect(m_clearLogBtn, &QPushButton::clicked, this, &MainWindow::onClearLog);

    QHBoxLayout *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(label);
    layout->addWidget(m_serverEdit);
    layout->addWidget(m_healthBtn);
    layout->addWidget(m_uploadBtn);
    layout->addWidget(m_listBtn);
    layout->addWidget(m_downloadBtn);
    layout->addWidget(m_clearLogBtn);
    layout->addStretch(1);

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
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
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
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择要上传的文件"));
    if (path.isEmpty()) {
        return;   // 用户取消
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("无法读取文件"), file.errorString());
        return;
    }
    const QByteArray data = file.readAll();
    file.close();

    const QString name = QFileInfo(path).fileName();

    QNetworkRequest req(buildUrl(QStringLiteral("/api/v1/files")));
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/octet-stream"));
    // 文件名按 RFC 3986 百分号编码后放进请求头，服务端会 urlDecode 还原
    req.setRawHeader("X-CV-Name", QUrl::toPercentEncoding(name));
    req.setHeader(QNetworkRequest::ContentLengthHeader, data.size());

    appendLog(QStringLiteral("POST"), req.url(), 0, 0,
              QStringLiteral("准备上传：%1（%2，请求头 X-CV-Name=%3）")
                  .arg(path, formatSize(data.size()),
                       QString::fromUtf8(QUrl::toPercentEncoding(name))));

    sendRequest(req, "POST", data);
}

void MainWindow::onListFiles()
{
    QNetworkRequest req(buildUrl(QStringLiteral("/api/v1/files")));
    sendRequest(req, "GET");
}

void MainWindow::onDownload()
{
    const QString id = currentFileId();
    if (id.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("请先选择文件"),
                                 QStringLiteral("请在列表中选中一行，再点击【下载选中文件】。"));
        return;
    }

    const QString name = currentFileName();
    const QString savePath =
        QFileDialog::getSaveFileName(this, QStringLiteral("保存到"), name);
    if (savePath.isEmpty()) {
        return;   // 用户取消
    }

    m_downloading = true;
    m_downloadId = id;
    m_downloadPath = savePath;

    QNetworkRequest req(buildUrl(QStringLiteral("/api/v1/files/%1/content").arg(id)));
    sendRequest(req, "GET");
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
//  网络
// ---------------------------------------------------------------------------
QUrl MainWindow::buildUrl(const QString &path) const
{
    QString base = m_serverEdit->text().trimmed();
    while (base.endsWith(QLatin1Char('/'))) {
        base.chop(1);
    }
    if (base.isEmpty() || (!base.startsWith(QStringLiteral("http://"))
                           && !base.startsWith(QStringLiteral("https://")))) {
        base = defaultServerUrl();   // 地址栏为空或格式不对时退回默认地址
    }
    return QUrl(base + path);
}

QNetworkReply *MainWindow::sendRequest(const QNetworkRequest &request, const QByteArray &verb,
                                       const QByteArray &body)
{
    QNetworkReply *reply = (verb == "GET") ? m_nam->get(request) : m_nam->post(request, body);

    // 记录起始时间，用于统计耗时
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
    const QString method =
        (reply->operation() == QNetworkAccessManager::GetOperation) ? QStringLiteral("GET")
                                                                    : QStringLiteral("POST");
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

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
                  QStringLiteral("[网络错误] %1").arg(reply->errorString()));
        m_downloading = false;   // 下载失败时清掉待落盘状态
        return;
    }

    appendLog(method, url, status, elapsed, formatBody(raw));

    // 按路径把响应分派给对应的处理逻辑
    const QString path = url.path();
    if (m_downloading && path.endsWith(QStringLiteral("/content"))) {
        handleDownloadReply(raw);
    } else if (path.endsWith(QStringLiteral("/api/v1/files")) && method == QStringLiteral("POST")) {
        handleUploadReply(raw);
    } else if (path.endsWith(QStringLiteral("/api/v1/files"))) {
        handleListReply(raw);
    }
    // /healthz 与 /api/v1/files/:id 只需要看日志，不做额外处理
}

void MainWindow::handleUploadReply(const QByteArray &raw)
{
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (!doc.isObject()) {
        return;
    }
    const QJsonObject obj = doc.object();

    const QString id = QString::number(obj.value(QStringLiteral("id")).toVariant().toLongLong());
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
           instant ? QStringLiteral("是") : QStringLiteral("否"), createdAt);
}

void MainWindow::handleListReply(const QByteArray &raw)
{
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (!doc.isObject()) {
        return;
    }
    const QJsonArray items = doc.object().value(QStringLiteral("items")).toArray();

    // 全量刷新：整段屏蔽信号，否则每插入一行都会触发一次选中变化 -> 预览请求
    // （N 行 = N 次「abort + 重发」，请求其实已经发出，服务端照样在跑）
    const bool wasBlocked = m_table->signalsBlocked();
    m_table->blockSignals(true);
    m_table->setRowCount(0);
    for (const QJsonValue &v : items) {
        if (!v.isObject()) {
            continue;
        }
        const QJsonObject o = v.toObject();
        const QString id = QString::number(o.value(QStringLiteral("id")).toVariant().toLongLong());

        QString instantText = QStringLiteral("-");   // 列表接口没有 instant 字段
        if (m_instantById.contains(id)) {
            instantText = m_instantById.value(id) ? QStringLiteral("是") : QStringLiteral("否");
        }
        addRow(id, o.value(QStringLiteral("name")).toString(),
               o.value(QStringLiteral("size")).toVariant().toLongLong(),
               o.value(QStringLiteral("hash")).toString(), instantText,
               o.value(QStringLiteral("created_at")).toVariant().toLongLong());
    }
    m_table->blockSignals(wasBlocked);

    // 刷新后没有选中行：同步清掉预览区，避免它还停留在刷新前那个文件上
    if (m_table->currentRow() < 0) {
        cancelPreview();
        m_previewId.clear();
        resetPreview();
    }
}

void MainWindow::handleDownloadReply(const QByteArray &raw)
{
    m_downloading = false;

    QFile out(m_downloadPath);
    if (!out.open(QIODevice::WriteOnly)) {
        appendLog(QStringLiteral("下载"), QUrl::fromLocalFile(m_downloadPath), 0, 0,
                  QStringLiteral("[落盘失败] %1").arg(out.errorString()));
        return;
    }
    const qint64 written = out.write(raw);
    out.close();

    appendLog(QStringLiteral("下载"), QUrl::fromLocalFile(m_downloadPath), 200, 0,
              QStringLiteral("已保存 id=%1 到 %2（写入 %3，响应 %4）")
                  .arg(m_downloadId, m_downloadPath, formatSize(written), formatSize(raw.size())));
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
    if (busy) {
        setCursor(Qt::BusyCursor);
    } else {
        unsetCursor();
    }
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
                        const QString &instantText, qint64 createdAt, bool doSelect)
{
    const int row = m_table->rowCount();
    m_table->insertRow(row);

    auto setCell = [this, row](int col, const QString &text) {
        QTableWidgetItem *item = new QTableWidgetItem(text);
        item->setToolTip(text);
        m_table->setItem(row, col, item);
        return item;
    };

    setCell(0, id)->setData(Qt::UserRole, id);   // id 同时存进 UserRole，下载时直接取
    setCell(1, name);
    setCell(2, formatSize(size))->setData(Qt::UserRole, size);              // 原始字节数
    setCell(3, formatTime(createdAt))->setData(Qt::UserRole, createdAt);    // 原始 created_at（毫秒）
    setCell(4, hash.left(8));
    setCell(5, instantText);

    // 选中行会触发 itemSelectionChanged -> onSelectionChanged -> 拉预览。
    // 刷新列表 / 上传完成时不需要预览，屏蔽信号只改当前行，不发请求
    if (doSelect) {
        const bool wasBlocked = m_table->signalsBlocked();
        m_table->blockSignals(true);
        m_table->selectRow(row);
        m_table->blockSignals(wasBlocked);
    }
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
    // 服务端 created_at 写的是 Unix 纪元毫秒（server/src/meta/file_repository.cpp 的
    // nowMillis），这里统一归一化成秒：超过 1e11（约公元 5138 年）即可认定是毫秒量级
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
    // 大响应（例如 6MB 下载）不参与 JSON 解析与全量转码，避免界面卡顿
    if (raw.size() > kMaxBodyInLog * 4) {
        return QStringLiteral("（响应体 %1，过大，仅展示大小）").arg(formatSize(raw.size()));
    }

    // 先尝试按 JSON 美化输出，失败则按纯文本/二进制截断展示
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
    QString text;
    if (err.error == QJsonParseError::NoError) {
        text = QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
    } else {
        text = QString::fromUtf8(raw);
    }

    if (text.size() > kMaxBodyInLog) {
        return QStringLiteral("%1\n…（响应过长，已省略 %2 字节）")
            .arg(text.left(kMaxBodyInLog), QString::number(text.size() - kMaxBodyInLog));
    }
    return text;
}
