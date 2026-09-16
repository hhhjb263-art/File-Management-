#include "core/config.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>

namespace cv {
namespace {

std::string envOr(const char* key, const std::string& def) {
  const char* v = std::getenv(key);
  return (v && *v) ? std::string(v) : def;
}

int toInt(const std::string& s, int def) {
  try {
    return std::stoi(s);
  } catch (...) {
    return def;
  }
}

std::size_t toSize(const std::string& s, std::size_t def) {
  try {
    return static_cast<std::size_t>(std::stoull(s));
  } catch (...) {
    return def;
  }
}

// 仅用于配置文件值的本地小写化（不引入额外依赖）
std::string toLowerLocal(const std::string& s) {
  std::string out = s;
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

// 解析 key=value 配置文件；忽略空行与 # 注释。
bool loadFile(const std::string& path, Config& cfg, std::string& err) {
  std::ifstream in(path);
  if (!in.is_open()) {
    err = "cannot open config: " + path;
    return false;
  }
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    auto pos = line.find('=');
    if (pos == std::string::npos) continue;
    std::string k = line.substr(0, pos);
    std::string v = line.substr(pos + 1);
    while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
    if (k == "data_dir") cfg.dataDir = v;
    else if (k == "files_root") cfg.filesRoot = v;
    else if (k == "listen") cfg.listenAddr = v;
    else if (k == "port") cfg.port = toInt(v, cfg.port);
    else if (k == "workers") cfg.workers = toInt(v, cfg.workers);
    else if (k == "chunk_size") cfg.chunkSize = toSize(v, cfg.chunkSize);
    else if (k == "log_file") cfg.logFile = v;
    else if (k == "log_level") cfg.logLevel = v;
    else if (k == "tls_port") cfg.tlsPort = toInt(v, cfg.tlsPort);
    else if (k == "tls_cert") cfg.tlsCert = v;
    else if (k == "tls_key") cfg.tlsKey = v;
    else if (k == "auth_token") cfg.authToken = v;
    else if (k == "http") cfg.httpEnabled = (toLowerLocal(v) != "off");
    else if (k == "data_key") cfg.dataKey = v;
  }
  return true;
}

}  // namespace

std::string configUsage(const char* program) {
  std::ostringstream os;
  os << "用法: " << program << " [选项]\n"
     << "  --config=<file>    读取 key=value 配置文件\n"
     << "  --data-dir=<path>  数据根目录（默认 /var/lib/cloudvault）\n"
     << "  --files-root=<path> 文件树允许根（默认 <data-dir>/files；客户端只能在其内建目录/上传/下载）\n"
     << "  --listen=<addr>    监听地址（默认 0.0.0.0）\n"
     << "  --port=<n>         监听端口（默认 8080）\n"
     << "  --workers=<n>      工作线程数（默认 4）\n"
     << "  --chunk-size=<n>   分块字节数（默认 5242880）\n"
     << "  --log-file=<path>  日志路径（默认输出到 stdout）\n"
     << "  --log-level=<lv>   debug|info|warn|error（默认 info）\n"
     << "  --tls-port=<n>     HTTPS 监听端口（默认 0 = 关闭；与 HTTP 可同时开）\n"
     << "  --tls-cert=<path>  PEM 证书路径（启用 HTTPS 必填）\n"
     << "  --tls-key=<path>   PEM 私钥路径（启用 HTTPS 必填）\n"
      << "  --auth-token=<s>   API Bearer Token（非空即启用鉴权，除 /healthz 外全接口强制）\n"
     << "  --http=<on|off>    明文 HTTP 监听开关（默认 on；off 时仅保留 HTTPS）\n"
     << "  --data-key=<path>  静态数据加密密钥文件（64 hex 或 32 字节原文；非空即启用 blob 加密）\n"
     << "  --help             显示帮助\n"
     << "\n环境变量：CV_CONFIG / CV_DATA_DIR / CV_FILES_ROOT / CV_LISTEN / CV_PORT /\n"
     << "          CV_WORKERS / CV_CHUNK_SIZE / CV_LOG_FILE / CV_LOG_LEVEL /\n"
     << "          CV_TLS_PORT / CV_TLS_CERT / CV_TLS_KEY / CV_AUTH_TOKEN / CV_HTTP / CV_DATA_KEY\n";
  return os.str();
}

Config loadConfig(int argc, char** argv) {
  Config cfg;

  std::string cfgFile = envOr("CV_CONFIG", "");
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a.rfind("--config=", 0) == 0) cfgFile = a.substr(9);
  }
  if (!cfgFile.empty()) {
    std::string err;
    if (!loadFile(cfgFile, cfg, err)) {
      // 配置文件不可读时仅告警，继续使用默认值
      std::fprintf(stderr, "[config] %s\n", err.c_str());
    }
  }

  cfg.dataDir = envOr("CV_DATA_DIR", cfg.dataDir);
  cfg.filesRoot = envOr("CV_FILES_ROOT", cfg.filesRoot);
  cfg.listenAddr = envOr("CV_LISTEN", cfg.listenAddr);
  cfg.port = toInt(envOr("CV_PORT", ""), cfg.port);
  cfg.workers = toInt(envOr("CV_WORKERS", ""), cfg.workers);
  cfg.chunkSize = toSize(envOr("CV_CHUNK_SIZE", ""), cfg.chunkSize);
  cfg.logFile = envOr("CV_LOG_FILE", cfg.logFile);
  cfg.logLevel = envOr("CV_LOG_LEVEL", cfg.logLevel);
  cfg.tlsPort = toInt(envOr("CV_TLS_PORT", ""), cfg.tlsPort);
  cfg.tlsCert = envOr("CV_TLS_CERT", cfg.tlsCert);
  cfg.tlsKey = envOr("CV_TLS_KEY", cfg.tlsKey);
  cfg.authToken = envOr("CV_AUTH_TOKEN", cfg.authToken);
  if (!envOr("CV_HTTP", "").empty()) {
    cfg.httpEnabled = (toLowerLocal(envOr("CV_HTTP", "")) != "off");
  }
  cfg.dataKey = envOr("CV_DATA_KEY", cfg.dataKey);

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto take = [&](const char* name) -> std::string {
      std::string key = std::string("--") + name + "=";
      if (a.rfind(key, 0) == 0) return a.substr(key.size());
      return std::string();
    };
    auto has = [&](const char* name) { return a == std::string("--") + name; };

    if (has("help")) {
      std::fprintf(stdout, "%s", configUsage(argv[0]).c_str());
      std::exit(0);
    }
    if (!take("data-dir").empty()) cfg.dataDir = take("data-dir");
    if (!take("files-root").empty()) cfg.filesRoot = take("files-root");
    if (!take("listen").empty()) cfg.listenAddr = take("listen");
    if (!take("port").empty()) cfg.port = toInt(take("port"), cfg.port);
    if (!take("workers").empty()) cfg.workers = toInt(take("workers"), cfg.workers);
    if (!take("chunk-size").empty()) cfg.chunkSize = toSize(take("chunk-size"), cfg.chunkSize);
    if (!take("log-file").empty()) cfg.logFile = take("log-file");
    if (!take("log-level").empty()) cfg.logLevel = take("log-level");
    if (!take("tls-port").empty()) cfg.tlsPort = toInt(take("tls-port"), cfg.tlsPort);
    if (!take("tls-cert").empty()) cfg.tlsCert = take("tls-cert");
    if (!take("tls-key").empty()) cfg.tlsKey = take("tls-key");
    if (!take("auth-token").empty()) cfg.authToken = take("auth-token");
    if (!take("http").empty()) cfg.httpEnabled = (toLowerLocal(take("http")) != "off");
    if (!take("data-key").empty()) cfg.dataKey = take("data-key");
  }

  if (cfg.workers < 1) cfg.workers = 1;
  if (cfg.port <= 0 || cfg.port > 65535) cfg.port = 8080;
  if (cfg.tlsPort < 0 || cfg.tlsPort > 65535) cfg.tlsPort = 0;
  if (cfg.chunkSize == 0) cfg.chunkSize = 5u * 1024u * 1024u;
  return cfg;
}

bool loadDataKeyFile(const std::string& path, std::vector<unsigned char>& out,
                      std::string& err) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    err = "cannot open data key file: " + path;
    return false;
  }
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  // 兼容 hex 文件末尾的换行 / 空白
  auto isWs = [](unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  };
  std::size_t a = 0, b = content.size();
  while (a < b && isWs(static_cast<unsigned char>(content[a]))) ++a;
  while (b > a && isWs(static_cast<unsigned char>(content[b - 1]))) --b;
  std::string s = content.substr(a, b - a);

  auto hexVal = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
  };

  if (s.size() == 64) {
    bool allHex = true;
    for (char c : s) {
      bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
      if (!ok) { allHex = false; break; }
    }
    if (allHex) {
      out.resize(32);
      for (std::size_t i = 0; i < 32; ++i) {
        out[i] = static_cast<unsigned char>(hexVal(s[2 * i]) * 16 + hexVal(s[2 * i + 1]));
      }
      return true;
    }
  }
  if (s.size() == 32) {
    out.assign(s.begin(), s.end());
    return true;
  }
  err = "data key must be 64 hex chars (32 bytes) or 32 raw bytes: " + path;
  return false;
}

}  // namespace cv
