#include "state.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

#include "log.hpp"
#include "paths.hpp"

namespace xdd {

namespace {

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string state_file(const char* name) {
    return nd_path(std::string("/data/adb/WY925976742/state/") + name);
}

}  // namespace

std::string runtime_dir() { return nd_path("/data/adb/WY925976742"); }

void write_current(const CurrentState& st) {
    FILE* f = std::fopen(state_file("current").c_str(), "w");
    if (!f) {
        log_line(LogLevel::Warn, "写 current 状态失败");
        return;
    }
    std::fprintf(f, "PKG=%s\nPROFILE=%s\nMODE=%s\nSINCE=%ld\n", st.pkg.c_str(),
                 st.profile.c_str(), st.mode.c_str(), st.since);
    std::fclose(f);
}

bool read_current(CurrentState& out) {
    std::ifstream f(state_file("current"));
    if (!f.is_open()) return false;
    std::string line;
    bool any = false;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = trim(line.substr(eq + 1));
        if (k == "PKG") out.pkg = v;
        else if (k == "PROFILE") out.profile = v;
        else if (k == "MODE") out.mode = v;
        else if (k == "SINCE") out.since = std::atol(v.c_str());
        any = true;
    }
    return any;
}

void clear_current() {
    std::ifstream f(state_file("current"));
    if (f.is_open()) f.close();
    std::remove(state_file("current").c_str());
}

void set_capability(const std::string& name, const std::string& status) {
    // 读-改-写：capability 文件小（<10 行），无需复杂并发控制
    std::ifstream in(state_file("capability"));
    std::ostringstream buf;
    bool replaced = false;
    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq != std::string::npos && line.substr(0, eq) == name) {
            buf << name << "=" << status << "\n";
            replaced = true;
        } else if (!line.empty()) {
            buf << line << "\n";
        }
    }
    if (!replaced) buf << name << "=" << status << "\n";

    FILE* f = std::fopen(state_file("capability").c_str(), "w");
    if (!f) {
        log_line(LogLevel::Warn, "写 capability 失败: " + name);
        return;
    }
    std::fputs(buf.str().c_str(), f);
    std::fclose(f);
}

std::string get_capability(const std::string& name) {
    std::ifstream in(state_file("capability"));
    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq != std::string::npos && line.substr(0, eq) == name) {
            return trim(line.substr(eq + 1));
        }
    }
    return std::string();
}

void write_heartbeat() {
    FILE* f = std::fopen(state_file("heartbeat").c_str(), "w");
    if (!f) return;
    std::fprintf(f, "%ld\n", static_cast<long>(std::time(nullptr)));
    std::fclose(f);
}

void write_live(const std::vector<std::pair<std::string, std::string>>& kv) {
    FILE* f = std::fopen(state_file("live").c_str(), "w");
    if (!f) return;
    for (const auto& p : kv) {
        std::fprintf(f, "%s=%s\n", p.first.c_str(), p.second.c_str());
    }
    std::fclose(f);
}

}  // namespace xdd
