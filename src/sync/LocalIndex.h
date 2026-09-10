/****************************************************************************
 * sync/LocalIndex.h —— 同步索引（SQLite）
 *
 * 记录"上次同步成功时"每个文件的状态（大小/修改时间/指纹/云端版本号），
 * 据此判断两端到底是哪一侧发生了变化。
 ****************************************************************************/
#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QString>

namespace cv {

class LocalIndex : public QObject
{
    Q_OBJECT
public:
    struct Entry
    {
        QString   pairId;
        QString   relPath;  // 相对同步根目录的路径，分隔符为 '/'
        bool      isDir = false;
        qint64    size  = 0;
        QDateTime mtime;    // 本地修改时间（UTC）
        QString   hash;     // 本地内容 SHA-256
        QString   rev;      // 同步完成时云端的 rev

        bool valid() const { return !relPath.isEmpty(); }
    };

    explicit LocalIndex(QObject *parent = nullptr);
    ~LocalIndex() override;

    bool open(const QString &dbFile);

    QHash<QString, Entry> entries(const QString &pairId) const; // key: relPath
    Entry                 entry(const QString &pairId, const QString &relPath) const;

    void put(const Entry &entry);
    void remove(const QString &pairId, const QString &relPath);
    void removePair(const QString &pairId);
    void clear();

private:
    QString m_dbFile;
};

} // namespace cv
