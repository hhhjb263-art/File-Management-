// 分享链接落地页渲染（纯函数，零依赖，可离线 harness 验证）。
//
// 设计要点：
//   * 仅依赖 <string>/<cstdint>/<ctime>/<cstdio>，不引用任何服务端内部类型，
//     因此可在 Windows 上单独编译并运行断言测试，无需启动服务端。
//   * 所有插入页面的动态值都必须先经 htmlEscape()，杜绝 XSS（文件名/错误文案等均不可信）。
//   * 下载链接里的提取码经 urlEncode()，避免把特殊字符直接拼进 URL。
//   * 单文件、内联 <style>、不引用任何外部资源（无 CDN / 无图片外链）。
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

namespace cv {
namespace share_page {

// HTML 实体转义：对 & < > " ' 五个字符做编码，防 XSS。
// 顺序必须先转 &，否则 &amp; 会被二次转义出 &amp;amp;。
inline std::string htmlEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out.push_back(static_cast<char>(c)); break;
    }
  }
  return out;
}

// 百分号编码（仅用于落地页内下载链接的 code 参数；与 util::urlEncode 同义，
// 这里独立实现以便 share_page.h 零依赖、可离线 harness）。保留 ASCII 安全字符，
// 其余按 %XX 编码。
inline std::string urlEncode(const std::string& s) {
  static const char* kUnreserved =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~";
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    bool safe = false;
    for (const char* p = kUnreserved; *p; ++p) {
      if (*p == static_cast<char>(c)) {
        safe = true;
        break;
      }
    }
    if (safe) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[(c >> 4) & 0xf]);
      out.push_back(kHex[c & 0xf]);
    }
  }
  return out;
}

// 人类可读体积（1024 进制）。
inline std::string formatSize(std::int64_t bytes) {
  if (bytes < 0) bytes = 0;
  static const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
  double v = static_cast<double>(bytes);
  int u = 0;
  while (v >= 1024.0 && u < 5) {
    v /= 1024.0;
    ++u;
  }
  char buf[64];
  if (u == 0) {
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(bytes));
  } else {
    std::snprintf(buf, sizeof(buf), "%.1f", v);
  }
  std::string out = buf;
  out += " ";
  out += kUnits[u];
  return out;
}

// 有效期文案：输入 Unix 毫秒，0 表示永久。以 UTC 绝对时间展示，
// 避免本地时区/区域差异导致 harness 结果不确定。
inline std::string formatExpiry(std::int64_t expiresAt) {
  if (expiresAt == 0) return "永久有效";
  std::time_t secs = static_cast<std::time_t>(expiresAt / 1000);
  std::tm t;
  // gmtime 返回指向静态缓冲的指针，立即拷贝到局部变量，避免多线程/重入问题。
  std::tm* pt = std::gmtime(&secs);
  if (!pt) return "未知";
  t = *pt;
  char buf[64];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M UTC", &t);
  return std::string(buf);
}

// 渲染分享落地页（纯函数）。
// 参数：
//   token        分享令牌（用于表单 action 与下载链接）
//   state        "code" 需要/错误提取码；"ok" 校验通过可下载；
//                "gone" 已过期/用尽；"missing" 链接不存在
//   name         文件名（仅 ok 态展示，内部已转义）
//   size         文件字节数（仅 ok 态展示）
//   expiresAt    有效期 Unix 毫秒，0=永久（仅 ok 态展示）
//   maxDownloads 下载次数上限，0=不限（仅 ok 态展示剩余次数）
//   downloads    已下载次数（仅 ok 态展示剩余次数）
//   verifiedCode 已校验通过的提取码（仅 ok 态用于下载链接，内部已 urlEncode）
//   error        错误文案（code/gone/missing 态展示，内部已转义）
//
// 下载链接形如 /s/<token>?code=<encoded>&dl=1：dl=1 是服务端内部提示，
// 告知路由“即便 Accept 含 text/html 也直接回附件”，从而让带 download 属性的
// 链接真正触发下载而不是再次落入落地页（浏览器点击 <a download> 仍会带 text/html）。
inline std::string renderSharePage(const std::string& token, const std::string& state,
                                   const std::string& name, std::int64_t size,
                                   std::int64_t expiresAt, std::int64_t maxDownloads,
                                   std::int64_t downloads, const std::string& verifiedCode,
                                   const std::string& error) {
  std::string body;
  if (state == "code") {
    body += "<h1>需要提取码</h1>\n";
    if (!error.empty()) body += "<p class=\"err\">" + htmlEscape(error) + "</p>\n";
    std::string formAction = "/s/" + htmlEscape(token);
    body += "<form method=\"GET\" action=\"" + formAction + "\">\n";
    body += "  <label for=\"code\">提取码</label>\n";
    body += "  <input id=\"code\" name=\"code\" type=\"text\" autocomplete=\"off\" "
            "autofocus required>\n";
    body += "  <button type=\"submit\">确认</button>\n";
    body += "</form>\n";
  } else if (state == "ok") {
    body += "<h1>" + htmlEscape(name) + "</h1>\n";
    body += "<ul class=\"meta\">\n";
    body += "  <li>大小：" + htmlEscape(formatSize(size)) + "</li>\n";
    body += "  <li>有效期：" + htmlEscape(formatExpiry(expiresAt)) + "</li>\n";
    std::string remain = (maxDownloads > 0)
                             ? std::to_string(std::max<std::int64_t>(0, maxDownloads - downloads))
                             : std::string("不限");
    body += "  <li>剩余下载次数：" + htmlEscape(remain) + "</li>\n";
    body += "</ul>\n";
    std::string dl =
        "/s/" + urlEncode(token) + "?code=" + urlEncode(verifiedCode) + "&dl=1";
    body += "<a class=\"btn\" href=\"" + dl + "\" download>下载</a>\n";
  } else if (state == "gone") {
    body += "<h1>链接已失效</h1>\n";
    body += "<p>" + htmlEscape(error.empty() ? "分享链接已过期或下载次数已用尽。" : error) +
            "</p>\n";
  } else {  // missing
    body += "<h1>链接不存在</h1>\n";
    body += "<p>" + htmlEscape(error.empty() ? "分享链接不存在或已撤销。" : error) +
            "</p>\n";
  }

  std::string html =
      "<!DOCTYPE html>\n"
      "<html lang=\"zh-CN\">\n"
      "<head>\n"
      "<meta charset=\"utf-8\">\n"
      "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
      "<title>CloudVault 分享</title>\n"
      "<style>\n"
      "  body{font-family:system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;"
      "background:#f5f6f8;color:#1f2329;margin:0;display:flex;min-height:100vh;"
      "align-items:center;justify-content:center}\n"
      "  .card{background:#fff;border-radius:12px;box-shadow:0 2px 12px "
      "rgba(0,0,0,.08);padding:32px 36px;max-width:420px;width:90%}\n"
      "  h1{font-size:20px;margin:0 0 16px}\n"
      "  .meta{list-style:none;padding:0;margin:0 0 20px;color:#5a6472}\n"
      "  .meta li{margin:6px 0}\n"
      "  .err{color:#d4380d;margin:0 0 12px}\n"
      "  label{display:block;font-size:13px;color:#5a6472;margin-bottom:6px}\n"
      "  input{width:100%;box-sizing:border-box;padding:10px 12px;border:1px solid "
      "#d0d3d9;border-radius:8px;font-size:15px}\n"
      "  button,.btn{display:inline-block;margin-top:14px;background:#2f6feb;color:#fff;"
      "border:none;border-radius:8px;padding:10px 20px;font-size:15px;text-decoration:"
      "none;cursor:pointer}\n"
      "</style>\n"
      "</head>\n"
      "<body>\n"
      "<div class=\"card\">\n" +
      body +
      "</div>\n"
      "</body>\n"
      "</html>\n";
  return html;
}

}  // namespace share_page
}  // namespace cv
