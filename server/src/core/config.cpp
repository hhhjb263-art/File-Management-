#include "core/config.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
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
     << "  --help             显示帮助\n"
     << "\n环境变量：CV_CONFIG / CV_DATA_DIR / CV_FILES_ROOT / CV_LISTEN / CV_PORT /\n"
     << "          CV_WORKERS / CV_CHUNK_SIZE / CV_LOG_FILE / CV_LOG_LEVEL\n";
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
  }

  if (cfg.workers < 1) cfg.workers = 1;
  if (cfg.port <= 0 || cfg.port > 65535) cfg.port = 8080;
  if (cfg.chunkSize == 0) cfg.chunkSize = 5u * 1024u * 1024u;
  return cfg;
}

}  // namespace cv
