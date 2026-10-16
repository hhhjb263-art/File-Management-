/****************************************************************************
 * app/virus_scan.h —— 恶意文件判定（策略 + 缓存 + 可注入的远端查询）
 *
 * ── 为什么是"按 hash 查"，不是"上传文件扫" ──
 * 1. **隐私**：本方案只把内容的 SHA-256 发出去，**文件本身不出服务器**。
 *    VirusTotal 官方对敏感场景的建议也是 hash 查询。
 * 2. **省额度**：免费层只有 4 请求/分钟、500 请求/天。
 * 3. **与架构契合**：本库是内容寻址（文件名即 SHA-256），同一份内容天然只存一份 ⇒
 *    `content_hash → 判定` 一张表就能让跨文件、跨用户的重复内容**只查一次**。
 *
 * ── 为什么远端查询做成"注入式回调" ──
 * 服务端是零第三方依赖 + 自研 POSIX socket。要真发 HTTPS 请求必须自己写 HTTP 客户端并依赖
 * OpenSSL（本项目里 OpenSSL 是**可选**编译项）。把它抽成回调的好处：
 *   · 策略/缓存/报文解析这些**真正容易写错**的部分，可以在没有网络、没有 OpenSSL 的情况下
 *     用假 fetcher 全量测试（见 lead_virus_scan_harness）；
 *   · 未编译 OpenSSL 的部署里，功能自动退化为"只读缓存"，而不是编译不过。
 *
 * ── 失败时放行还是拒绝 ──
 * 默认**放行 + 记日志**（fail-open），与配额那轮同一取舍：第三方服务的抖动不该让整个网盘不可用。
 * 想更严可以配成 fail-closed。这不是安全边界，是一道尽力而为的闸门 —— 代码里不许把它当墙。
 ****************************************************************************/
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "core/json.h"
#include "meta/db.h"
#include "net/http_client.h"

namespace cv {
namespace scan {

enum class Verdict
{
    Unknown = 0,    // 远端库里没有这个样本（**不代表安全**）
    Clean = 1,
    Suspicious = 2,
    Malicious = 3
};

enum class Decision
{
    Allow,        // 正常放行
    AllowWithWarn,// 放行但要告警（可疑 / 未收录）
    Block         // 拒绝
};

struct Result
{
    Verdict      verdict = Verdict::Unknown;
    Decision     decision = Decision::Allow;
    int          enginesMalicious = 0;
    int          enginesSuspicious = 0;
    int          enginesTotal = 0;
    std::string  detail;        // 给用户看的中文原因
    bool         fromCache = false;
    bool         fetcherFailed = false;   // 远端查询失败（结果不可信，按策略放行/拒绝）
};

// ── 远端查询的抽象 ──
// 返回 true = 查询成功，verdict/engines* 已填好；false = 失败（网络/鉴权/报文异常）。
// 用 std::function 注入：生产接 HTTPS 客户端，测试接假实现。
using Fetcher = std::function<bool(const std::string &hash, Result &out, std::string &err)>;

struct Policy
{
    int  blockThreshold = 1;   // malicious 引擎数 ≥ 此值即拒绝（默认 1：私有盘宁严勿松，误报可手工放行）
    bool failOpen = true;      // 远端查询失败时：true 放行（默认）/ false 拒绝
};

namespace detail {

// 从内容哈希取库表键（本库哈希恒为 64 位小写十六进制）
inline bool validHash(const std::string &h)
{
    if (h.size() != 64)
        return false;
    for (char c : h)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}

// 只允许哈希进 SQL（不是用户可控字符串，且先经 validHash 白名单校验）
inline std::string q(const std::string &s)
{
    std::string o;
    o.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '\'')
            o.push_back('\'');
        o.push_back(c);
    }
    return o;
}

}   // namespace detail

// 建表（幂等）。挂在 meta 库上 —— 判定是按**内容**走的，与 owner / 文件名无关。
inline bool initSchema(Db &db, std::string &err)
{
    return db.exec("CREATE TABLE IF NOT EXISTS file_scan ("
                   "  content_hash      TEXT PRIMARY KEY,"
                   "  verdict           INTEGER NOT NULL DEFAULT 0,"
                   "  engines_malicious INTEGER NOT NULL DEFAULT 0,"
                   "  engines_suspicious INTEGER NOT NULL DEFAULT 0,"
                   "  engines_total     INTEGER NOT NULL DEFAULT 0,"
                   "  detail            TEXT NOT NULL DEFAULT '',"
                   "  scanned_at        INTEGER NOT NULL"
                   ")",
                   err);
}

// 读缓存。found=false 表示没查过（**不等于 Clean** —— 这个区分很重要，
// 否则"未收录"会被当成"已确认安全"而永远不再复查）。
inline bool cacheGet(Db &db, const std::string &hash, Result &out, bool &found, std::string &err)
{
    found = false;
    if (!detail::validHash(hash)) {
        err = "invalid content hash";
        return false;
    }
    std::vector<std::vector<std::string>> rows;
    if (!db.query("SELECT verdict, engines_malicious, engines_suspicious, engines_total, detail "
                  "FROM file_scan WHERE content_hash = '" +
                      detail::q(hash) + "'",
                  rows, err))
        return false;
    if (rows.empty() || rows[0].size() < 5)
        return true;   // 没查过

    found              = true;
    out.fromCache      = true;
    out.verdict        = static_cast<Verdict>(std::stoll(rows[0][0]));
    out.enginesMalicious  = static_cast<int>(std::stoll(rows[0][1]));
    out.enginesSuspicious = static_cast<int>(std::stoll(rows[0][2]));
    out.enginesTotal      = static_cast<int>(std::stoll(rows[0][3]));
    out.detail            = rows[0][4];
    return true;
}

inline bool cachePut(Db &db, const std::string &hash, const Result &r, std::int64_t nowMillis,
                     std::string &err)
{
    if (!detail::validHash(hash)) {
        err = "invalid content hash";
        return false;
    }
    return db.exec("INSERT INTO file_scan "
                   "(content_hash, verdict, engines_malicious, engines_suspicious, engines_total, "
                   " detail, scanned_at) VALUES ('" +
                       detail::q(hash) + "', " + std::to_string(static_cast<int>(r.verdict)) +
                       ", " + std::to_string(r.enginesMalicious) + ", " +
                       std::to_string(r.enginesSuspicious) + ", " +
                       std::to_string(r.enginesTotal) + ", '" + detail::q(r.detail) + "', " +
                       std::to_string(nowMillis) + ") "
                       "ON CONFLICT(content_hash) DO UPDATE SET "
                       "verdict = excluded.verdict, engines_malicious = excluded.engines_malicious, "
                       "engines_suspicious = excluded.engines_suspicious, "
                       "engines_total = excluded.engines_total, detail = excluded.detail, "
                       "scanned_at = excluded.scanned_at",
                   err);
}

// 纯策略：把"远端判定"翻译成"拦/放"。与 IO 无关，最容易写错也最容易测。
inline Decision decide(Verdict v, int enginesMalicious, const Policy &p)
{
    // ⚠️ **阈值是唯一的判定权威**，不要再无条件 `if (v == Malicious) Block` ——
    //    那样一来阈值就永远不生效（解析器只要看到 1 个引擎报毒就把 verdict 标成 Malicious，
    //    阈值判断会变成死代码）。我第一版正是这么写的，写用例时才发现。
    if (enginesMalicious > 0 && enginesMalicious >= p.blockThreshold)
        return Decision::Block;
    // 报了毒但没到阈值 / 只被标可疑 → 放行但**必须留痕**，不要静默放过
    if (v == Verdict::Malicious || v == Verdict::Suspicious)
        return Decision::AllowWithWarn;
    if (v == Verdict::Unknown)
        return Decision::AllowWithWarn;   // 未收录 ≠ 安全
    return Decision::Allow;
}

inline std::string verdictName(Verdict v)
{
    switch (v) {
    case Verdict::Clean:      return "安全";
    case Verdict::Suspicious: return "可疑";
    case Verdict::Malicious:  return "恶意";
    case Verdict::Unknown:    return "未收录";
    }
    return "未知";
}

// 主入口：先查缓存，未命中才调 fetcher，成功后回写缓存。
// fetcher 为空 ⇒ 视为"远端不可用"（未编译 OpenSSL 的部署即此情形）。
inline Result scan(Db &db, const std::string &hash, const Fetcher &fetcher, const Policy &policy,
                   std::int64_t nowMillis, bool cacheEnabled, std::string &err)
{
    Result r;

    if (cacheEnabled) {
        bool found = false;
        std::string cerr;
        if (cacheGet(db, hash, r, found, cerr) && found) {
            r.decision = decide(r.verdict, r.enginesMalicious, policy);
            return r;
        }
        // 缓存读失败不致命：继续走远端（下面会给 err 让调用方记日志）
        if (!cerr.empty())
            err = cerr;
    }

    Result remote;
    std::string ferr;
    const bool ok = fetcher && fetcher(hash, remote, ferr);
    if (!ok) {
        r.fetcherFailed = true;
        r.verdict       = Verdict::Unknown;
        r.detail        = fetcher ? ("远端查询失败：" + ferr) : "未配置远端查询（未编译 HTTPS 支持？）";
        r.decision      = policy.failOpen ? Decision::AllowWithWarn : Decision::Block;
        return r;
    }

    remote.fromCache = false;
    remote.decision  = decide(remote.verdict, remote.enginesMalicious, policy);

    if (cacheEnabled) {
        std::string perr;
        // 只缓存"有结论"的判定：Unknown 也缓存（避免同一天反复打无用请求），
        // 但**不**把缓存当作安全证明 —— 语义上它只是"查过"。
        cachePut(db, hash, remote, nowMillis, perr);
    }
    return remote;
}

// ── VirusTotal v3 `GET /files/{sha256}` 的响应解析（**纯函数**）──
// 单独抽出来是因为：第三方报文的形状是**外部契约**，最容易因为对方加字段/改嵌套而悄悄读成 0，
// 而"读成 0"在安全语义上等于"没报毒" —— 恰好是最危险的方向。所以它必须能脱离网络单测。
//
// 期望形状（只取需要的字段）：
//   {"data":{"attributes":{"last_analysis_stats":{"harmless":n,"malicious":n,"suspicious":n,
//                                                 "undetected":n,"timeout":n,"failure":n}}}}
inline bool parseVirusTotal(const std::string &body, Result &out, std::string &err)
{
    json::Value root;
    if (!json::parse(body, root, err)) {
        err = "远端报文不是合法 JSON：" + err;
        return false;
    }
    const json::Value *data = root.find("data");
    if (!data) {
        err = "远端报文缺少 data";
        return false;
    }
    const json::Value *attrs = data->find("attributes");
    if (!attrs) {
        err = "远端报文缺少 data.attributes";
        return false;
    }
    const json::Value *stats = attrs->find("last_analysis_stats");
    if (!stats) {
        err = "远端报文缺少 last_analysis_stats";
        return false;
    }

    const auto num = [&stats](const char *k) -> int {
        const json::Value *v = stats->find(k);
        return v ? static_cast<int>(v->numberValue(0.0)) : 0;
    };
    out.enginesMalicious  = num("malicious");
    out.enginesSuspicious = num("suspicious");
    const int harmless   = num("harmless");
    const int undetected = num("undetected");
    out.enginesTotal     = out.enginesMalicious + out.enginesSuspicious + harmless + undetected;

    if (out.enginesMalicious > 0)
        out.verdict = Verdict::Malicious;
    else if (out.enginesSuspicious > 0)
        out.verdict = Verdict::Suspicious;
    else if (out.enginesTotal > 0)
        out.verdict = Verdict::Clean;
    else
        out.verdict = Verdict::Unknown;   // 引擎全为 0：多半是"未收录"，别当成安全

    out.detail = "引擎 " + std::to_string(out.enginesMalicious) + " 报毒 / " +
                 std::to_string(out.enginesSuspicious) + " 可疑 / 共 " +
                 std::to_string(out.enginesTotal);
    return true;
}

// ── HTTP 响应 → 判定（**纯函数**，可脱离网络测试）──
// 语义要点（这里最容易把"正常空结果"误判成"远端故障"）：
//   200  → 解析报文；解析失败按"远端不可信"处理 → 返回 false（走 fail-open + 留痕）
//   404  → **未收录，这是有效结论，不是失败**。VT 对没见过的样本就返回 404；
//          若把它当失败，日志会被"远端故障"刷满，真正的故障反而看不见。
//   401/403 → 凭据无效/无权限 → 失败（要让运维看见原因）
//   429  → 触发限额 → 失败（免费层 4/分钟、500/天）
//   其它 → 失败
inline bool verdictFromHttpResponse(const net::HttpResponse &resp, Result &out, std::string &err)
{
    if (resp.status == 200)
        return parseVirusTotal(resp.body, out, err);

    if (resp.status == 404) {
        out.verdict = Verdict::Unknown;
        out.detail  = "远端未收录该内容（未收录 ≠ 安全）";
        return true;
    }
    if (resp.status == 401 || resp.status == 403) {
        err = "远端凭据无效或无权限（HTTP " + std::to_string(resp.status) +
              "），请检查 --scan-api-key";
        return false;
    }
    if (resp.status == 429) {
        err = "触发远端查询限额（HTTP 429）—— 免费层为 4 请求/分钟、500 请求/天";
        return false;
    }
    err = "远端返回 HTTP " + std::to_string(resp.status);
    return false;
}

// 组装一个走 VirusTotal v3 的 fetcher。apiKey 为空 ⇒ 返回空 fetcher（= 不查询）。
// ⚠️ 未编译 OpenSSL 时仍然返回一个 fetcher，但它一律失败并说明原因 ——
//    这样"没有 HTTPS 能力"会以明确原因进入 fail-open 路径，而不是编译不过或静默通过。
inline Fetcher makeVirusTotalFetcher(const std::string &apiKey, int timeoutMs)
{
    if (apiKey.empty())
        return Fetcher();
    return [apiKey, timeoutMs](const std::string &hash, Result &out, std::string &err) -> bool {
        if (!detail::validHash(hash)) {
            err = "invalid content hash";
            return false;
        }
#ifdef CV_HAVE_OPENSSL
        net::HttpResponse resp;
        std::string       herr;
        // 只发哈希，**文件本身不出服务器**（隐私 + 省额度）
        const std::vector<std::pair<std::string, std::string>> hdrs = {
            {"x-apikey", apiKey},
            {"accept", "application/json"},
        };
        if (!net::httpsGet("www.virustotal.com", "/api/v3/files/" + hash, hdrs, timeoutMs, resp,
                           herr)) {
            err = herr;
            return false;
        }
        return verdictFromHttpResponse(resp, out, err);
#else
        err = "本二进制未编译 OpenSSL 支持，无法发起 HTTPS 查询";
        return false;
#endif
    };
}

}   // namespace scan
}   // namespace cv
