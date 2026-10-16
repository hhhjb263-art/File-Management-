/****************************************************************************
 * app/db_backup.h —— 元数据库一致性热备（header-only）
 *
 * ── 为什么需要 ──
 * 全部文件名、目录树、标签、分享、版本、账号都在两个 SQLite 文件里：
 *   · <dataDir>/meta/cloudvault.db （文件元数据）
 *   · <dataDir>/meta/users.db      （账号/会话）
 * 在这之前**没有任何备份手段** —— 这两个文件一坏，文件内容虽在磁盘上，
 * 但整棵目录树与归属关系就没了。这是整个系统里最集中的单点风险。
 *
 * ── 为什么用 VACUUM INTO 而不是拷文件 ──
 * 直接 `cp` 一个正在被写的 SQLite（尤其 WAL 模式）会拿到**撕裂的、不一致的**副本：
 * 主库文件与 -wal 不同步，恢复出来可能缺事务甚至打不开。
 * `VACUUM INTO 'dst'` 由 SQLite 自己产生一份**单事务一致快照**，且：
 *   · 源库以**只读**方式打开即可（服务端在跑也能安全备份）；
 *   · 产出的副本是**紧凑**的（无空洞、无 WAL 残留），拿回来直接能用；
 *   · 不会改源库一个字节。
 * 需要 SQLite ≥ 3.27（2019-02 起）；本仓库链接的是 3.35.5。
 *
 * ── 恢复方式 ──
 * 停服 → 用备份文件覆盖 <dataDir>/meta/ 下的同名文件 → 重启。
 * 注意 -wal / -shm 残留必须先删掉，否则旧 WAL 会盖掉恢复的数据。
 ****************************************************************************/
#pragma once

#include <sqlite3.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "core/config.h"

namespace fs = std::filesystem;

namespace cv {
namespace dbbackup {

struct Stats
{
    std::vector<std::string> written;   // 成功产出的备份文件
    std::vector<std::string> skipped;   // 源库不存在 → 跳过（不算错误）
    std::int64_t             bytes = 0; // 备份总字节数
};

// 单库转义：把路径里的单引号翻倍，避免拼进 SQL 时被截断/注入
inline std::string quoteLiteral(const std::string &s)
{
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\'')
            o.push_back('\'');
        o.push_back(c);
    }
    return o;
}

// 备份单个 SQLite 文件。要求：src 存在；dst **不存在**（VACUUM INTO 拒绝覆盖，
// 这里也刻意不替调用方删旧文件 —— "备份"最忌讳悄悄盖掉上一份）。
inline bool backupFile(const std::string &src, const std::string &dst, std::string &err)
{
    std::error_code ec;
    if (!fs::exists(src, ec)) {
        err = "源数据库不存在：" + src;
        return false;
    }
    if (fs::exists(dst, ec)) {
        err = "备份目标已存在（拒绝覆盖）：" + dst;
        return false;
    }
    // 目标目录必须先建好，VACUUM INTO 不会替我们建
    fs::create_directories(fs::path(dst).parent_path(), ec);
    if (ec) {
        err = "无法创建备份目录：" + fs::path(dst).parent_path().string();
        return false;
    }

    sqlite3 *db = nullptr;
    // 只读打开源库：既保证不改动生产数据，也让"服务端正在运行"时备份依然安全
    if (sqlite3_open_v2(src.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        err = std::string("打开源数据库失败：") + (db ? sqlite3_errmsg(db) : "unknown");
        if (db)
            sqlite3_close(db);
        return false;
    }

    const std::string sql = "VACUUM INTO '" + quoteLiteral(dst) + "'";
    char             *msg = nullptr;
    const int         rc  = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &msg);
    if (rc != SQLITE_OK) {
        err = std::string("VACUUM INTO 失败：") + (msg ? msg : sqlite3_errmsg(db));
        if (msg)
            sqlite3_free(msg);
        sqlite3_close(db);
        return false;
    }
    sqlite3_close(db);
    return true;
}

// 备份本服务端的全部数据库到 outDir（目录必须不存在或为空目录也可）。
inline bool backupAll(const Config &cfg, const std::string &outDir, Stats &st, std::string &err)
{
    st = Stats();

    struct Item
    {
        const char *label;
        std::string src;
    };
    const std::vector<Item> items = {
        {"cloudvault.db", cfg.dbPath()},
        {"users.db", cfg.usersDbPath()},
    };

    std::error_code ec;
    fs::create_directories(outDir, ec);
    if (ec) {
        err = "无法创建备份目录：" + outDir;
        return false;
    }

    for (const Item &it : items) {
        if (!fs::exists(it.src, ec)) {
            // 全新安装可能还没有 users.db —— 跳过而不是报错
            st.skipped.push_back(std::string(it.label) + "（源不存在）");
            continue;
        }
        const std::string dst = outDir + "/" + it.label;
        std::string       ferr;
        if (!backupFile(it.src, dst, ferr)) {
            err = ferr;
            return false;
        }
        st.written.push_back(dst);
        const auto sz = fs::file_size(dst, ec);
        if (!ec)
            st.bytes += static_cast<std::int64_t>(sz);
    }
    return true;
}

}   // namespace dbbackup
}   // namespace cv
