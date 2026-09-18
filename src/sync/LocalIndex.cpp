#include "LocalIndex.h"

#include "core/Logging.h"

#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QVariant>

namespace cv {
namespace {

// 每个线程使用独立连接（QSqlDatabase 不能跨线程共享）
QSqlDatabase connectionFor(const QString &file)
{
    const QString name = QStringLiteral("cv_sync_%1").arg(qintptr(QThread::currentThreadId()));

    if (QSqlDatabase::contains(name))
        return QSqlDatabase::database(name);

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
    db.setDatabaseName(file);
    if (!db.open()) {
        qWarning() << "同步索引打开失败:" << db.lastError().text();
        return db;
    }

    QSqlQuery q(db);
    q.exec(QStringLiteral(R"(
        CREATE TABLE IF NOT EXISTS sync_index (
            pair_id  TEXT NOT NULL,
            rel_path TEXT NOT NULL,
            is_dir   INTEGER NOT NULL DEFAULT 0,
            size     INTEGER NOT NULL DEFAULT 0,
            mtime    TEXT,
            hash     TEXT,
            rev      TEXT,
            PRIMARY KEY (pair_id, rel_path)
        );
    )"));
    if (q.lastError().isValid())
        qWarning() << "创建同步索引表失败:" << q.lastError().text();

    return db;
}

QString timeToString(const QDateTime &dt)
{
    return dt.isValid() ? dt.toUTC().toString(Qt::ISODateWithMs) : QString();
}

QDateTime timeFromString(const QString &s)
{
    return s.isEmpty() ? QDateTime() : QDateTime::fromString(s, Qt::ISODateWithMs);
}

} // namespace

LocalIndex::LocalIndex(QObject *parent)
    : QObject(parent)
{}

LocalIndex::~LocalIndex() = default;

bool LocalIndex::open(const QString &dbFile)
{
    m_dbFile = dbFile;
    QSqlDatabase db = connectionFor(m_dbFile);
    return db.isOpen();
}

QHash<QString, LocalIndex::Entry> LocalIndex::entries(const QString &pairId) const
{
    QHash<QString, Entry> out;
    QSqlDatabase          db = connectionFor(m_dbFile);
    if (!db.isOpen())
        return out;

    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT rel_path, is_dir, size, mtime, hash, rev "
                             "FROM sync_index WHERE pair_id = :pair"));
    q.bindValue(QStringLiteral(":pair"), pairId);
    if (!q.exec()) {
        qWarning() << "读取同步索引失败:" << q.lastError().text();
        return out;
    }

    while (q.next()) {
        Entry e;
        e.pairId = pairId;
        e.relPath = q.value(0).toString();
        e.isDir   = q.value(1).toBool();
        e.size    = q.value(2).toLongLong();
        e.mtime   = timeFromString(q.value(3).toString());
        e.hash    = q.value(4).toString();
        e.rev     = q.value(5).toString();
        out.insert(e.relPath, e);
    }
    return out;
}

LocalIndex::Entry LocalIndex::entry(const QString &pairId, const QString &relPath) const
{
    Entry        e;
    QSqlDatabase db = connectionFor(m_dbFile);
    if (!db.isOpen())
        return e;

    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT is_dir, size, mtime, hash, rev FROM sync_index "
                             "WHERE pair_id = :pair AND rel_path = :path"));
    q.bindValue(QStringLiteral(":pair"), pairId);
    q.bindValue(QStringLiteral(":path"), relPath);
    if (q.exec() && q.next()) {
        e.pairId  = pairId;
        e.relPath = relPath;
        e.isDir   = q.value(0).toBool();
        e.size    = q.value(1).toLongLong();
        e.mtime   = timeFromString(q.value(2).toString());
        e.hash    = q.value(3).toString();
        e.rev     = q.value(4).toString();
    }
    return e;
}

void LocalIndex::put(const Entry &entry)
{
    QSqlDatabase db = connectionFor(m_dbFile);
    if (!db.isOpen())
        return;

    QSqlQuery q(db);
    q.prepare(QStringLiteral("INSERT INTO sync_index (pair_id, rel_path, is_dir, size, mtime, hash, rev) "
                             "VALUES (:pair, :path, :dir, :size, :mtime, :hash, :rev) "
                             "ON CONFLICT(pair_id, rel_path) DO UPDATE SET "
                             "is_dir = excluded.is_dir, size = excluded.size, "
                             "mtime = excluded.mtime, hash = excluded.hash, rev = excluded.rev"));
    q.bindValue(QStringLiteral(":pair"), entry.pairId);
    q.bindValue(QStringLiteral(":path"), entry.relPath);
    q.bindValue(QStringLiteral(":dir"), entry.isDir ? 1 : 0);
    q.bindValue(QStringLiteral(":size"), entry.size);
    q.bindValue(QStringLiteral(":mtime"), timeToString(entry.mtime));
    q.bindValue(QStringLiteral(":hash"), entry.hash);
    q.bindValue(QStringLiteral(":rev"), entry.rev);
    if (!q.exec())
        qWarning() << "写入同步索引失败:" << q.lastError().text();
}

void LocalIndex::remove(const QString &pairId, const QString &relPath)
{
    QSqlDatabase db = connectionFor(m_dbFile);
    if (!db.isOpen())
        return;

    QSqlQuery q(db);
    q.prepare(QStringLiteral("DELETE FROM sync_index WHERE pair_id = :pair AND rel_path = :path"));
    q.bindValue(QStringLiteral(":pair"), pairId);
    q.bindValue(QStringLiteral(":path"), relPath);
    q.exec();
}

void LocalIndex::removePair(const QString &pairId)
{
    QSqlDatabase db = connectionFor(m_dbFile);
    if (!db.isOpen())
        return;

    QSqlQuery q(db);
    q.prepare(QStringLiteral("DELETE FROM sync_index WHERE pair_id = :pair"));
    q.bindValue(QStringLiteral(":pair"), pairId);
    q.exec();
}

void LocalIndex::clear()
{
    QSqlDatabase db = connectionFor(m_dbFile);
    if (!db.isOpen())
        return;

    QSqlQuery q(db);
    q.exec(QStringLiteral("DELETE FROM sync_index"));
}

} // namespace cv
