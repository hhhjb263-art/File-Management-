// 远端文件树选择对话框实现（详见 FileTreeDialog.h）
//
// 打开即并发拉取 GET /api/v1/dirs 与 GET /api/v1/files，客户端合成目录树：
//   - 根节点固定为 "/（根目录）"，path = ""（= 根目录）
//   - 目录按 "/" 拆分逐级建节点；dirs 只列 "a/b" 而没有 "a" 时自动补 "a"
//   - 文件按其 dir 挂到对应目录节点下（dir="" 直接挂根）
// 列：名称 | 类型 | 大小 | 修改时间。单选（SelectRows）。刷新保留展开状态。

#include "FileTreeDialog.h"
#include "MainWindow.h"   // 复用 validRelPathInput / formatSize / formatTime（已设为 public）

#include <QAbstractItemView>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QShowEvent>
#include <QStyle>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

namespace {

// 在 parent 的直接子节点里找 path 与给定值相等的目录节点
QTreeWidgetItem *findChildDir(QTreeWidgetItem *parent, const QString &path)
{
    if (!parent) {
        return nullptr;
    }
    for (int i = 0; i < parent->childCount(); ++i) {
        QTreeWidgetItem *c = parent->child(i);
        if (c->data(0, Qt::UserRole).toString() == path) {
            return c;
        }
    }
    return nullptr;
}

}   // namespace

FileTreeDialog::FileTreeDialog(const QUrl &dirsUrl, const QUrl &filesUrl, Mode mode,
                               const QString &initialPath, bool allowCreateDir, QWidget *parent)
    : QDialog(parent), m_dirsUrl(dirsUrl), m_filesUrl(filesUrl), m_mode(mode),
      m_initialPath(initialPath), m_allowCreateDir(allowCreateDir)
{
    setWindowTitle(mode == Mode::SelectDir ? QStringLiteral("选择目录")
                                           : QStringLiteral("选择文件"));
    resize(600, 500);

    auto *vbox = new QVBoxLayout(this);

    m_status = new QLabel(QStringLiteral("准备中…"), this);
    m_status->setWordWrap(true);
    vbox->addWidget(m_status);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(4);
    m_tree->setHeaderLabels({QStringLiteral("名称"), QStringLiteral("类型"),
                             QStringLiteral("大小"), QStringLiteral("修改时间")});
    m_tree->setAlternatingRowColors(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setRootIsDecorated(true);
    vbox->addWidget(m_tree, 1);

    auto *hbox = new QHBoxLayout();
    if (m_allowCreateDir) {
        m_createBtn = new QPushButton(QStringLiteral("新建文件夹"), this);
        connect(m_createBtn, &QPushButton::clicked, this, &FileTreeDialog::onCreateFolder);
        hbox->addWidget(m_createBtn);
    }
    auto *expandBtn = new QPushButton(QStringLiteral("全部展开"), this);
    connect(expandBtn, &QPushButton::clicked, this, &FileTreeDialog::onExpandAll);
    hbox->addWidget(expandBtn);
    auto *collapseBtn = new QPushButton(QStringLiteral("全部折叠"), this);
    connect(collapseBtn, &QPushButton::clicked, this, &FileTreeDialog::onCollapseAll);
    hbox->addWidget(collapseBtn);
    auto *refreshBtn = new QPushButton(QStringLiteral("刷新"), this);
    connect(refreshBtn, &QPushButton::clicked, this, &FileTreeDialog::onRefresh);
    hbox->addWidget(refreshBtn);
    m_retryBtn = new QPushButton(QStringLiteral("重试"), this);
    m_retryBtn->setVisible(false);
    connect(m_retryBtn, &QPushButton::clicked, this, &FileTreeDialog::onRetry);
    hbox->addWidget(m_retryBtn);

    hbox->addStretch(1);

    if (m_allowCreateDir) {
        // 浏览/新建模式：用【关闭】代替【确定】（真实动作是建文件夹）
        m_closeBtn = new QPushButton(QStringLiteral("关闭"), this);
        m_closeBtn->setDefault(true);
        connect(m_closeBtn, &QPushButton::clicked, this, &FileTreeDialog::onClose);
        hbox->addWidget(m_closeBtn);
    } else {
        m_okBtn = new QPushButton(QStringLiteral("确定"), this);
        m_okBtn->setDefault(true);
        m_okBtn->setEnabled(false);   // 未选合法节点前禁用
        connect(m_okBtn, &QPushButton::clicked, this, &FileTreeDialog::onOk);
        hbox->addWidget(m_okBtn);
    }

    vbox->addLayout(hbox);

    connect(m_tree, &QTreeWidget::currentItemChanged, this, &FileTreeDialog::onCurrentChanged);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, &FileTreeDialog::onItemDoubleClicked);
    connect(m_tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *it) {
        if (isDirItem(it)) {
            m_expandedPaths.insert(itemPath(it));
        }
    });
    connect(m_tree, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *it) {
        if (isDirItem(it)) {
            m_expandedPaths.remove(itemPath(it));
        }
    });

    m_nam = new QNetworkAccessManager(this);
}

FileTreeDialog::~FileTreeDialog() = default;

void FileTreeDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    loadData();   // 打开即拉两个列表
}

void FileTreeDialog::onCurrentChanged()
{
    applySelectionEnabled();
}

void FileTreeDialog::onItemDoubleClicked(QTreeWidgetItem *item, int /*column*/)
{
    if (isDirItem(item)) {
        item->setExpanded(!item->isExpanded());   // 双击目录 -> 切换展开
    }
}

void FileTreeDialog::onOk()
{
    QTreeWidgetItem *cur = m_tree->currentItem();
    if (!cur) {
        return;
    }
    const QString kind = cur->data(0, Qt::UserRole + 1).toString();
    if (m_mode == Mode::SelectFile && kind != QStringLiteral("file")) {
        return;
    }
    if (m_mode == Mode::SelectDir && kind != QStringLiteral("dir")) {
        return;
    }
    m_selectedPath = itemPath(cur);
    m_selectedIsFile = (kind == QStringLiteral("file"));
    accept();
}

void FileTreeDialog::onClose()
{
    reject();   // 浏览/新建模式：关闭即可（建文件夹已在对话框内完成）
}

void FileTreeDialog::onExpandAll()
{
    m_tree->expandAll();
}

void FileTreeDialog::onCollapseAll()
{
    m_tree->collapseAll();
}

void FileTreeDialog::onRefresh()
{
    loadData();
}

void FileTreeDialog::onRetry()
{
    loadData();
}

void FileTreeDialog::onCreateFolder()
{
    QTreeWidgetItem *cur = m_tree->currentItem();
    if (!cur || !isDirItem(cur)) {
        QMessageBox::warning(this, QStringLiteral("请先选择目录"),
                             QStringLiteral("请先选中一个目录（或根目录），再在其下新建文件夹。"));
        return;
    }
    const QString parentPath = itemPath(cur);   // 根 = ""

    bool ok = false;
    const QString name = QInputDialog::getText(
                             this, QStringLiteral("新建文件夹"),
                             QStringLiteral("文件夹名称（单段，不含 '/'，如 backup）："),
                             QLineEdit::Normal, QString(), &ok)
                             .trimmed();
    if (!ok || name.isEmpty()) {
        return;   // 用户取消
    }
    QString why;
    if (!MainWindow::validRelPathInput(name, &why)) {   // 校验单段名字（与服务端规则一致）
        QMessageBox::warning(this, QStringLiteral("名称不合法"), why);
        return;
    }
    const QString full = parentPath.isEmpty() ? name : parentPath + QLatin1Char('/') + name;

    QJsonObject body;
    body.insert(QStringLiteral("path"), full);
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    QNetworkRequest req(m_dirsUrl);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setHeader(QNetworkRequest::ContentLengthHeader, payload.size());
    QNetworkReply *r = m_nam->post(req, payload);
    connect(r, &QNetworkReply::finished, this, [this, r, full]() {
        const int st = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ne = (r->error() != QNetworkReply::NoError);
        const QByteArray raw = r->readAll();
        r->deleteLater();
        handleCreateReply(st, ne, raw, full);
    });
}

void FileTreeDialog::loadData()
{
    m_pending = 0;
    m_loadError = false;
    m_loadErrorMsg.clear();
    m_dirsItems = QJsonArray();
    m_filesItems = QJsonArray();
    setStatus(QStringLiteral("正在加载文件树…"));
    m_tree->setEnabled(false);
    m_retryBtn->setVisible(false);
    if (m_okBtn) {
        m_okBtn->setEnabled(false);
    }

    // GET /api/v1/dirs
    {
        QNetworkRequest req(m_dirsUrl);
        QNetworkReply *r = m_nam->get(req);
        ++m_pending;
        connect(r, &QNetworkReply::finished, this, [this, r]() {
            const int st = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool ne = (r->error() != QNetworkReply::NoError);
            const QByteArray raw = r->readAll();
            r->deleteLater();
            --m_pending;
            handleDirsReply(st, ne, raw);
        });
    }
    // GET /api/v1/files
    {
        QNetworkRequest req(m_filesUrl);
        QNetworkReply *r = m_nam->get(req);
        ++m_pending;
        connect(r, &QNetworkReply::finished, this, [this, r]() {
            const int st = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool ne = (r->error() != QNetworkReply::NoError);
            const QByteArray raw = r->readAll();
            r->deleteLater();
            --m_pending;
            handleFilesReply(st, ne, raw);
        });
    }
}

void FileTreeDialog::handleDirsReply(int status, bool networkError, const QByteArray &raw)
{
    if (networkError || status != 200) {
        m_loadError = true;
        m_loadErrorMsg = networkError ? QStringLiteral("网络错误，无法拉取目录列表。")
                                      : QStringLiteral("拉取目录列表失败（HTTP %1）。").arg(status);
    } else {
        QJsonParseError perr;
        const QJsonDocument doc = QJsonDocument::fromJson(raw, &perr);
        if (doc.isNull() || !doc.isObject()) {
            m_loadError = true;
            m_loadErrorMsg = QStringLiteral("目录列表响应解析失败：%1").arg(perr.errorString());
        } else {
            m_dirsItems = doc.object().value(QStringLiteral("items")).toArray();
        }
    }
    maybeBuildTree();
}

void FileTreeDialog::handleFilesReply(int status, bool networkError, const QByteArray &raw)
{
    if (networkError || status != 200) {
        m_loadError = true;
        m_loadErrorMsg = networkError ? QStringLiteral("网络错误，无法拉取文件列表。")
                                      : QStringLiteral("拉取文件列表失败（HTTP %1）。").arg(status);
    } else {
        QJsonParseError perr;
        const QJsonDocument doc = QJsonDocument::fromJson(raw, &perr);
        if (doc.isNull() || !doc.isObject()) {
            m_loadError = true;
            m_loadErrorMsg = QStringLiteral("文件列表响应解析失败：%1").arg(perr.errorString());
        } else {
            m_filesItems = doc.object().value(QStringLiteral("items")).toArray();
        }
    }
    maybeBuildTree();
}

void FileTreeDialog::maybeBuildTree()
{
    if (m_pending > 0) {
        return;   // 等另一个列表也回来
    }
    if (m_loadError) {
        setStatus(m_loadErrorMsg);
        m_retryBtn->setVisible(true);
        m_tree->setEnabled(false);
        return;
    }
    buildTree();
}

void FileTreeDialog::buildTree()
{
    m_tree->clear();

    QTreeWidgetItem *root = new QTreeWidgetItem(m_tree->invisibleRootItem());
    root->setText(0, QStringLiteral("/（根目录）"));
    root->setData(0, Qt::UserRole, QString());         // path = ""
    root->setData(0, Qt::UserRole + 1, QStringLiteral("dir"));
    root->setText(1, QStringLiteral("目录"));
    root->setText(2, QStringLiteral("-"));
    root->setText(3, QStringLiteral("-"));
    root->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));

    // 目录节点（按 '/' 逐级补齐中间目录）
    for (const QJsonValue &v : m_dirsItems) {
        const QString p = v.toString();
        if (!p.isEmpty()) {
            ensureDir(p);
        }
    }
    // 文件节点
    for (const QJsonValue &v : m_filesItems) {
        if (!v.isObject()) {
            continue;
        }
        const QJsonObject o = v.toObject();
        const QString name = o.value(QStringLiteral("name")).toString();
        const QString dir = o.value(QStringLiteral("dir")).toString();
        const qint64 size = o.value(QStringLiteral("size")).toVariant().toLongLong();
        const qint64 createdAt = o.value(QStringLiteral("created_at")).toVariant().toLongLong();
        addFile(dir, name, size, createdAt);
    }

    m_tree->setEnabled(true);
    m_retryBtn->setVisible(false);

    // 展开状态：首次默认展开根 + 一级目录；之后按 m_expandedPaths 恢复
    if (m_firstBuild) {
        root->setExpanded(true);
        for (int i = 0; i < root->childCount(); ++i) {
            QTreeWidgetItem *c = root->child(i);
            if (isDirItem(c)) {
                c->setExpanded(true);
            }
        }
        m_firstBuild = false;
    } else {
        restoreExpanded();
    }

    if (m_dirsItems.isEmpty() && m_filesItems.isEmpty()) {
        setStatus(QStringLiteral("（服务器上没有已记录的目录/文件）"));
    } else {
        highlightInitial();
        applySelectionEnabled();
    }
}

QTreeWidgetItem *FileTreeDialog::rootItem() const
{
    QTreeWidgetItem *inv = m_tree->invisibleRootItem();
    return inv->childCount() > 0 ? inv->child(0) : nullptr;
}

QTreeWidgetItem *FileTreeDialog::ensureDir(const QString &path)
{
    QTreeWidgetItem *parent = rootItem();
    const QStringList segs = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString acc;
    for (const QString &seg : segs) {
        acc = acc.isEmpty() ? seg : acc + QLatin1Char('/') + seg;
        QTreeWidgetItem *child = findChildDir(parent, acc);
        if (!child) {
            child = new QTreeWidgetItem(parent);
            child->setText(0, seg);
            child->setData(0, Qt::UserRole, acc);
            child->setData(0, Qt::UserRole + 1, QStringLiteral("dir"));
            child->setText(1, QStringLiteral("目录"));
            child->setText(2, QStringLiteral("-"));
            child->setText(3, QStringLiteral("-"));
            child->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
        }
        parent = child;
    }
    return parent;
}

void FileTreeDialog::addFile(const QString &dir, const QString &name, qint64 size, qint64 createdAt)
{
    QTreeWidgetItem *parent = dir.isEmpty() ? rootItem() : ensureDir(dir);
    auto *item = new QTreeWidgetItem(parent);
    item->setText(0, name);
    const QString filePath = dir.isEmpty() ? name : dir + QLatin1Char('/') + name;
    item->setData(0, Qt::UserRole, filePath);
    item->setData(0, Qt::UserRole + 1, QStringLiteral("file"));
    item->setData(0, Qt::UserRole + 2, QVariant::fromValue<qint64>(size));
    item->setData(0, Qt::UserRole + 3, QVariant::fromValue<qint64>(createdAt));
    item->setText(1, QStringLiteral("文件"));
    item->setText(2, size >= 0 ? MainWindow::formatSize(size) : QStringLiteral("-"));
    item->setText(3, MainWindow::formatTime(createdAt));
    item->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
}

void FileTreeDialog::setStatus(const QString &text)
{
    m_status->setText(text);
}

void FileTreeDialog::applySelectionEnabled()
{
    QTreeWidgetItem *cur = m_tree->currentItem();
    if (!cur) {
        if (m_okBtn) {
            m_okBtn->setEnabled(false);
        }
        setStatus(m_mode == Mode::SelectFile ? QStringLiteral("请选择要下载的文件。")
                                             : QStringLiteral("请选择目录，或选中根目录。"));
        return;
    }

    const QString kind = cur->data(0, Qt::UserRole + 1).toString();
    const QString path = itemPath(cur);

    if (m_mode == Mode::SelectFile) {
        const bool file = (kind == QStringLiteral("file"));
        if (m_okBtn) {
            m_okBtn->setEnabled(file);
        }
        if (file) {
            const qint64 sz = cur->data(0, Qt::UserRole + 2).toLongLong();
            setStatus(QStringLiteral("已选择：%1（文件，%2）").arg(path, MainWindow::formatSize(sz)));
        } else {
            setStatus(QStringLiteral("当前选中不是文件，请选择文件。"));
        }
    } else {
        const bool dir = (kind == QStringLiteral("dir"));   // 含根目录
        if (m_okBtn) {
            m_okBtn->setEnabled(dir);
        }
        if (dir) {
            setStatus(path.isEmpty() ? QStringLiteral("已选择目录：/（根目录）")
                                     : QStringLiteral("已选择目录：%1").arg(path));
        } else {
            setStatus(QStringLiteral("当前选中不是目录，请选择目录或根目录。"));
        }
    }
}

bool FileTreeDialog::isDirItem(QTreeWidgetItem *item) const
{
    return item && item->data(0, Qt::UserRole + 1).toString() == QStringLiteral("dir");
}

QString FileTreeDialog::itemPath(QTreeWidgetItem *item) const
{
    return item ? item->data(0, Qt::UserRole).toString() : QString();
}

void FileTreeDialog::restoreExpanded()
{
    QTreeWidgetItemIterator it(m_tree);
    while (*it) {
        QTreeWidgetItem *it2 = *it;
        if (isDirItem(it2) && m_expandedPaths.contains(itemPath(it2))) {
            it2->setExpanded(true);
        }
        ++it;
    }
}

void FileTreeDialog::highlightInitial()
{
    if (m_initialPath.isEmpty()) {
        QTreeWidgetItem *root = rootItem();
        if (root) {
            m_tree->setCurrentItem(root);
        }
        return;
    }
    QTreeWidgetItem *match = nullptr;
    QTreeWidgetItemIterator it(m_tree);
    while (*it) {
        if ((*it)->data(0, Qt::UserRole).toString() == m_initialPath) {
            match = *it;
            break;
        }
        ++it;
    }
    if (match) {
        // 展开 match 及其全部祖先，确保可见
        QTreeWidgetItem *p = match;
        while (p) {
            p->setExpanded(true);
            p = p->parent();
        }
        m_tree->setCurrentItem(match);
        m_tree->scrollToItem(match);
        return;
    }
    // 下载模式：精确匹配不到文件时，退而高亮其父目录并展开
    if (m_mode == Mode::SelectFile) {
        const QString parentPath = m_initialPath.section(QLatin1Char('/'), 0, -2);
        if (!parentPath.isEmpty()) {
            QTreeWidgetItem *pp = nullptr;
            QTreeWidgetItemIterator it2(m_tree);
            while (*it2) {
                if ((*it2)->data(0, Qt::UserRole).toString() == parentPath) {
                    pp = *it2;
                    break;
                }
                ++it2;
            }
            if (pp) {
                pp->setExpanded(true);
                m_tree->setCurrentItem(pp);
                m_tree->scrollToItem(pp);
            }
        }
    }
}

void FileTreeDialog::handleCreateReply(int status, bool networkError, const QByteArray &raw,
                                       const QString &newPath)
{
    if (networkError || (status != 201 && status != 200)) {
        const QString msg = networkError
            ? QStringLiteral("网络错误，创建目录失败。")
            : QStringLiteral("创建目录失败（HTTP %1）：%2")
                  .arg(status)
                  .arg(MainWindow::formatBody(raw));
        QMessageBox::warning(this, QStringLiteral("创建目录失败"), msg);
        return;
    }
    if (status == 200) {
        QMessageBox::information(this, QStringLiteral("创建目录"),
                                 QStringLiteral("目录已存在（幂等成功）。"));
    }
    // 成功：刷新树并选中新目录（选中的节点高亮由 highlightInitial 基于 m_initialPath 完成）
    m_initialPath = newPath;
    loadData();
}
