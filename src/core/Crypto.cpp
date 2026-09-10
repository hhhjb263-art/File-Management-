#include "Crypto.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QRandomGenerator>

namespace cv {
namespace {

constexpr int kBlockSize = 1024 * 1024; // 1MB

} // namespace

QString Crypto::sha256File(const QString &path,
                           const std::function<void(qint64, qint64)> &progress,
                           bool *canceled)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "无法打开文件计算哈希:" << path << file.errorString();
        return {};
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    const qint64       total = file.size();
    qint64             read  = 0;

    while (!file.atEnd()) {
        if (canceled && *canceled)
            return {};

        const QByteArray block = file.read(kBlockSize);
        if (block.isEmpty())
            break;
        hash.addData(block);
        read += block.size();

        if (progress)
            progress(read, total);
    }

    return QString::fromLatin1(hash.result().toHex());
}

QString Crypto::sha256Hex(const QByteArray &data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

QString Crypto::sha256Hex(const QString &text)
{
    return sha256Hex(text.toUtf8());
}

QString Crypto::randomHex(int bytes)
{
    QByteArray       buf(bytes, Qt::Uninitialized);
    QRandomGenerator *rng = QRandomGenerator::global();
    for (int i = 0; i < bytes; ++i)
        buf[i] = static_cast<char>(rng->bounded(256));
    return QString::fromLatin1(buf.toHex());
}

QString Crypto::randomCode(int length)
{
    // 去掉 0/O/1/I/l 等易混淆字符
    static const QString alphabet = QStringLiteral("23456789ABCDEFGHJKMNPQRSTUVWXYZ");
    QString out;
    out.reserve(length);
    for (int i = 0; i < length; ++i)
        out.append(alphabet.at(QRandomGenerator::global()->bounded(alphabet.size())));
    return out;
}

QString Crypto::randomToken(int bytes)
{
    return randomHex(bytes);
}

} // namespace cv
