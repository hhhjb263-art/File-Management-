/****************************************************************************
 * core/AppPaths.h —— 应用各类目录与文件的固定位置
 ****************************************************************************/
#pragma once

#include <QString>

namespace cv {

class AppPaths
{
public:
    // 便携模式：exe 同目录存在 portable.txt ⇒ 配置/缓存/日志/下载都放在 exe 旁边
    static bool    isPortable();
    static QString rootDir();      // %LOCALAPPDATA%/CloudVault
    static QString configDir();    // 配置
    static QString logDir();       // 日志
    static QString cacheDir();     // 缓存（上传临时分片等）
    static QString dataDir();      // 本地引擎的数据存储根目录
    static QString downloadDir();  // 默认下载目录

    static QString settingsFile(); // settings.ini
    static QString indexDbFile();  // 同步索引 sync.db
    static QString localDbFile();  // 本地引擎元数据 db.json

    static void ensureDirs();
};

} // namespace cv
