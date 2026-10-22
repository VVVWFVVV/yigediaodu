#include "log.hpp"

#include <cstdio>
#include <ctime>
#include <string>

namespace xdd {

static FILE* g_log_file = nullptr;

void log_open(const std::string& file) {
    log_close();
    if (!file.empty()) {
        g_log_file = std::fopen(file.c_str(), "a");
    }
}

void log_close() {
    if (g_log_file) {
        std::fclose(g_log_file);
        g_log_file = nullptr;
    }
}

std::string now_ts() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d-%02d %02d:%02d:%02d",
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buf);
}

void log_line(LogLevel lv, const std::string& msg) {
    const char* tag = (lv == LogLevel::Warn) ? "WARN" : (lv == LogLevel::Error) ? "ERROR" : "INFO";
    std::string line = now_ts() + " [" + tag + "] " + msg;
    if (g_log_file) {
        std::fputs(line.c_str(), g_log_file);
        std::fputc('\n', g_log_file);
        std::fflush(g_log_file);
    }
    std::fputs(line.c_str(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

}  // namespace xdd
