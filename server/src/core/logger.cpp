#include "core/logger.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace cv {
namespace log {
namespace {

std::mutex g_mutex;
std::ofstream g_file;
Level g_min = Level::Info;
bool g_toStdout = true;

std::string timestamp() {
  auto now = std::chrono::system_clock::now();
  auto t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) %
            1000;
  std::ostringstream os;
  os << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0')
     << std::setw(3) << ms.count();
  return os.str();
}

}  // namespace

Level parseLevel(const std::string& name) {
  if (name == "debug") return Level::Debug;
  if (name == "warn" || name == "warning") return Level::Warn;
  if (name == "error") return Level::Error;
  return Level::Info;
}

const char* levelName(Level lv) {
  switch (lv) {
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO ";
    case Level::Warn: return "WARN ";
    case Level::Error: return "ERROR";
  }
  return "INFO ";
}

void init(const std::string& file, Level min) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_min = min;
  if (file.empty()) {
    g_toStdout = true;
    return;
  }
  g_file.open(file, std::ios::app);
  g_toStdout = !g_file.is_open();
}

void write(Level lv, const char* file, int line, const std::string& msg) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (lv < g_min) return;
  std::ostringstream os;
  os << timestamp() << " [" << levelName(lv) << "] " << file << ':' << line
     << " | " << msg << '\n';
  std::string lineText = os.str();
  if (g_toStdout || !g_file.is_open()) {
    std::fwrite(lineText.data(), 1, lineText.size(), stdout);
    std::fflush(stdout);
  }
  if (g_file.is_open()) {
    g_file << lineText;
    g_file.flush();
  }
}

}  // namespace log
}  // namespace cv
