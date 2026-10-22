#include "config.hpp"

#include <dirent.h>
#include <fstream>
#include <stdlib.h>
#include <unistd.h>

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

std::string strip_quotes(const std::string& s) {
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                          (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

}  // namespace

ConfParseResult conf_parse_file(const std::string& path) {
    ConfParseResult r;
    std::ifstream f(path);
    if (!f.is_open()) return r;

    std::string line;
    int line_no = 0;
    while (std::getline(f, line)) {
        ++line_no;
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;

        size_t eq = t.find('=');
        if (eq == std::string::npos) {
            r.bad_lines.push_back("L" + std::to_string(line_no) + ": " + t);
            continue;
        }
        std::string key = trim(t.substr(0, eq));
        std::string val = strip_quotes(trim(t.substr(eq + 1)));
        if (key.empty()) {
            r.bad_lines.push_back("L" + std::to_string(line_no) + ": " + t);
            continue;
        }
        r.kv.emplace_back(key, val);
    }
    return r;
}

bool conf_get(const Conf& kv, const std::string& key, std::string& out) {
    // 后写覆盖先写：倒序找第一个命中
    for (auto it = kv.rbegin(); it != kv.rend(); ++it) {
        if (it->first == key) {
            out = it->second;
            return true;
        }
    }
    return false;
}

std::string conf_get_or(const Conf& kv, const std::string& key, const std::string& def) {
    std::string v;
    return conf_get(kv, key, v) ? v : def;
}

bool conf_has(const Conf& kv, const std::string& key) {
    std::string v;
    return conf_get(kv, key, v);
}

std::string module_dir() {
    // 测试与 fcctl 通过环境变量指定；设备上由 /proc/self/exe 推导
    const char* env = std::getenv("XDD_MODDIR");
    if (env && *env) return std::string(env);

    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return std::string();
    buf[n] = '\0';
    std::string p(buf);
    // .../libs/arm64-v8a/xindiaodud -> 模块根
    size_t pos = p.find("/libs/arm64-v8a/xindiaodud");
    if (pos != std::string::npos) return p.substr(0, pos);
    return std::string();
}

// ============================================================
// 模块配置寻址：注意——这些是模块自身文件（随 zip 分发），
// host 与 device 两种模式都走真实路径，绝不加 XD_ROOT 前缀。
// XD_ROOT 前缀只用于设备状态路径（/sys、/proc、/data/adb 运行时）。
// ============================================================

static std::string cfg_root() {
    static const std::string root = module_dir() + "/config";
    return root;
}

std::string settings_path() { return cfg_root() + "/settings.conf"; }

std::string profile_path(const std::string& name) {
    return cfg_root() + "/profiles/" + name + ".conf";
}

std::string perapp_path(const std::string& pkg) {
    return cfg_root() + "/perapp/" + pkg + ".conf";
}

std::vector<std::string> perapp_list() {
    std::vector<std::string> out;
    std::string dir = cfg_root() + "/perapp";
    DIR* d = ::opendir(dir.c_str());
    if (!d) return out;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        std::string name = e->d_name;
        if (name.size() > 5 && name.substr(name.size() - 5) == ".conf") {
            out.push_back(name.substr(0, name.size() - 5));
        }
    }
    ::closedir(d);
    return out;
}

}  // namespace xdd
