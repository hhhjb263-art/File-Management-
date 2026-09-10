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
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
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
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace {

// 日志里单条响应最多展示的字节数，避免 6MB 大文件把日志区刷爆
constexpr int kMaxBodyInLog = 4096;

// 服务端默认端口
constexpr int kDefaultPort = 8080;

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

    m_table = new QTableWidget(0, 5, box);
    m_table->setHorizontalHeaderLabels(
        {QStringLiteral("ID"), QStringLiteral("名称"), QStringLiteral("大小"),
         QStringLiteral("Hash 前 8 位"), QStringLiteral("是否秒传")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setColumnWidth(0, 70);
    m_table->setColumnWidth(1, 260);
    m_table->setColumnWidth(2, 100);
    m_table->setColumnWidth(3, 130);
    m_table->setColumnWidth(4, 100);

    QVBoxLayout *layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_table);

    return box;
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

    // 上传成功后直接把这条补进列表，省得用户再点一次【列出文件】
    addRow(id, obj.value(QStringLiteral("name")).toString(),
           obj.value(QStringLiteral("size")).toVariant().toLongLong(),
           obj.value(QStringLiteral("hash")).toString(),
           instant ? QStringLiteral("是") : QStringLiteral("否"));
}

void MainWindow::handleListReply(const QByteArray &raw)
{
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (!doc.isObject()) {
        return;
    }
    const QJsonArray items = doc.object().value(QStringLiteral("items")).toArray();

    m_table->setRowCount(0);   // 全量刷新
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
               o.value(QStringLiteral("hash")).toString(), instantText);
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

void MainWindow::addRow(const QString &id, const QString &name, qint64 size, const QString &hash,
                        const QString &instantText)
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
    setCell(2, formatSize(size));
    setCell(3, hash.left(8));
    setCell(4, instantText);

    m_table->selectRow(row);
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
