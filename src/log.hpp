#pragma once
#include <string>

namespace xdd {

enum class LogLevel { Info, Warn, Error };

// 打开日志文件（追加写）；之后 log_line 同时写文件与 stderr
void log_open(const std::string& file);
void log_close();

// 统一日志：落文件 + stderr；stderr 由 service.sh 重定向进 daemon.out
void log_line(LogLevel lv, const std::string& msg);

// "MM-DD HH:MM:SS"
std::string now_ts();

}  // namespace xdd
