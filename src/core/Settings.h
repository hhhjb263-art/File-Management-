/****************************************************************************
 * core/Settings.h —— 本地配置持久化（settings.ini，单例）
 ****************************************************************************/
#pragma once

#include "Types.h"

#include <QObject>
#include <QSettings>

namespace cv {

class Settings : public QObject
{
    Q_OBJECT
public:
    static Settings &instance();

    // ---- 后端 ----
    QString backendMode() const; // "local" 本地引擎 / "remote" 远程服务
    void    setBackendMode(const QString &mode);
    QString serverUrl() const;
    void    setServerUrl(const QString &url);
    QString token() const;
    void    setToken(const QString &token);
    QString lastUser() const;
    void    setLastUser(const QString &user);

    // ---- 外观 ----
    bool darkMode() const;
    void setDarkMode(bool dark);

    // ---- 常规 ----
    bool    launchAtLogin() const;
    void    setLaunchAtLogin(bool on);
    bool    closeToTray() const;
    void    setCloseToTray(bool on);
    QString downloadDir() const;
    void    setDownloadDir(const QString &dir);

    // ---- 传输 ----
    int    concurrency() const;   // 同时传输数
    void   setConcurrency(int n);
    int    chunkSizeMB() const;   // 分块大小（MB）
    void   setChunkSizeMB(int mb);

    // ---- 同步 ----
    bool autoSync() const;
    void setAutoSync(bool on);
    int  fullScanIntervalMin() const;
    void setFullScanIntervalMin(int minutes);

    // ---- 自动刷新（加法式）----
    // 文件列表 / 任务队列的自动刷新配置。interval 合法范围 5..3600 秒，
    // 越界一律钳制到边界（setter 落盘前钳制）。
    bool autoRefreshEnabled() const;        // 默认 true
    void setAutoRefreshEnabled(bool on);
    int  autoRefreshIntervalSec() const;    // 默认 30
    void setAutoRefreshIntervalSec(int sec);

    QVector<SyncPair> syncPairs() const;
    void              setSyncPairs(const QVector<SyncPair> &pairs);

    void sync();

private:
    explicit Settings(QObject *parent = nullptr);

    QSettings m_settings;
};

} // namespace cv
