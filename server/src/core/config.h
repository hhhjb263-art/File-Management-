#pragma once

#include <cstddef>
#include <string>

namespace cv {

// 服务端配置。优先级：命令行 > 环境变量 > 配置文件 > 内置默认值。
struct Config {
  std::string dataDir = "/var/lib/cloudvault";  // 数据根目录（元数据/临时/内容库）
  // 文件树允许根（客户端可见的目录树根）。为空则取 <dataDir>/files。
  // 显式配置而非由进程运行目录推导 —— 服务端从任何 cwd 启动，落点都一致。
  std::string filesRoot = "";
  std::string listenAddr = "0.0.0.0";
  int port = 8080;
  int workers = 4;                 // HTTP 工作线程数
  std::size_t chunkSize = 5u * 1024u * 1024u;  // 分块大小，默认 5MB
  std::string logFile;             // 为空则输出到 stdout
  std::string logLevel = "info";
  // TLS（HTTPS）双模式：tlsPort > 0 且证书/私钥齐全时额外开 HTTPS 监听；
  // 均不配置则纯 HTTP。HTTP 与 HTTPS 可同时运行，共享同一套路由。
  int tlsPort = 0;                 // 0 = 关闭 HTTPS
  std::string tlsCert;             // PEM 证书路径
  std::string tlsKey;              // PEM 私钥路径

  std::string dbPath() const { return dataDir + "/meta/cloudvault.db"; }
  std::string blobRoot() const { return dataDir + "/blobs"; }
  std::string tmpRoot() const { return dataDir + "/tmp"; }
  // 文件树允许根（唯一可被客户端创建目录 / 上传 / 下载的物理目录）
  std::string filesRootDir() const {
    return filesRoot.empty() ? dataDir + "/files" : filesRoot;
  }
};

// 解析 argc/argv、环境变量（CV_*）与 --config 指定的 key=value 文件。
Config loadConfig(int argc, char** argv);

// 命令行帮助文本。
std::string configUsage(const char* program);

}  // namespace cv
