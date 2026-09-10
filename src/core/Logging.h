/****************************************************************************
 * core/Logging.h —— 日志：同时输出到 stderr 与日志文件
 ****************************************************************************/
#pragma once

#include <QString>

namespace cv {

class Logging
{
public:
    // 安装消息处理器，日志文件位于 dir/cloudvault.log
    static void install(const QString &dir);
    static QString logFilePath();
};

} // namespace cv
