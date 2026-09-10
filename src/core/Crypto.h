/****************************************************************************
 * core/Crypto.h —— 哈希与随机串（秒传指纹、分享码、令牌）
 ****************************************************************************/
#pragma once

#include <QString>
#include <functional>

namespace cv {

class Crypto
{
public:
    // 分块计算文件 SHA-256，返回小写十六进制；canceled 可中断
    static QString sha256File(const QString &path,
                              const std::function<void(qint64, qint64)> &progress = {},
                              bool *canceled = nullptr);

    static QString sha256Hex(const QByteArray &data);
    static QString sha256Hex(const QString &text);

    static QString randomHex(int bytes = 16);         // 32 位十六进制 ID
    static QString randomCode(int length = 6);        // 分享提取码（去掉易混字符）
    static QString randomToken(int bytes = 32);       // 会话令牌
};

} // namespace cv
