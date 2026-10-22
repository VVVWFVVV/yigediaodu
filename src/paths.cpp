#include "paths.hpp"

#include <cstdlib>

namespace xdd {

static std::string read_env(const char* key) {
    const char* v = std::getenv(key);
    return v ? std::string(v) : std::string();
}

const std::string& xd_root() {
    // 只读一次：运行期改环境变量不生效，符合"开机一套环境"的预期
    static const std::string root = read_env("XD_ROOT");
    return root;
}

bool host_mode() { return !xd_root().empty(); }

std::string nd_path(const std::string& abs) {
    const std::string& r = xd_root();
    if (r.empty()) return abs;
    if (!abs.empty() && abs[0] == '/') return r + abs;
    // 相对路径不透传（调用方传错时应原样返回，让 open 失败暴露问题）
    return abs;
}

std::string nd_path(const char* abs) { return nd_path(std::string(abs)); }

}  // namespace xdd
