#pragma once

#include <cstddef>
#include <string>

namespace cv {

// 服务端配置。优先级：命令行 > 环境变量 > 配置文件 > 内置默认值。
struct Config {
  std::string dataDir = "/var/lib/cloudvault";  // 数据根目录
  std::string listenAddr = "0.0.0.0";
  int port = 8080;
  int workers = 4;                 // HTTP 工作线程数
  std::size_t chunkSize = 5u * 1024u * 1024u;  // 分块大小，默认 5MB
  std::string logFile;             // 为空则输出到 stdout
  std::string logLevel = "info";

  std::string dbPath() const { return dataDir + "/meta/cloudvault.db"; }
  std::string blobRoot() const { return dataDir + "/blobs"; }
  std::string tmpRoot() const { return dataDir + "/tmp"; }
};

// 解析 argc/argv、环境变量（CV_*）与 --config 指定的 key=value 文件。
Config loadConfig(int argc, char** argv);

// 命令行帮助文本。
std::string configUsage(const char* program);

}  // namespace cv
