#pragma once

#include <sstream>
#include <string>

namespace cv {
namespace log {

enum class Level { Debug = 0, Info, Warn, Error };

// 字符串转等级，未知值回退 Info。
Level parseLevel(const std::string& name);
const char* levelName(Level lv);

// file 为空则输出到 stdout；否则追加写入文件。
void init(const std::string& file, Level min);

// 线程安全地输出一行：时间 等级 文件:行号 | 消息
void write(Level lv, const char* file, int line, const std::string& msg);

}  // namespace log
}  // namespace cv

// 支持流式写法：CV_LOG_INFO("上传完成 id=" << id);
#define CV_LOG_AT(level, msg)                                            \
  do {                                                                   \
    std::ostringstream _cv_log_ss;                                       \
    _cv_log_ss << msg;                                                   \
    ::cv::log::write(level, __FILE__, __LINE__, _cv_log_ss.str());       \
  } while (false)

#define CV_LOG_DEBUG(msg) CV_LOG_AT(::cv::log::Level::Debug, msg)
#define CV_LOG_INFO(msg) CV_LOG_AT(::cv::log::Level::Info, msg)
#define CV_LOG_WARN(msg) CV_LOG_AT(::cv::log::Level::Warn, msg)
#define CV_LOG_ERROR(msg) CV_LOG_AT(::cv::log::Level::Error, msg)
