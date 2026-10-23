#pragma once
#include <string>
#include <utility>
#include <vector>

namespace xdd {

// 行式配置：保留顺序的键值对（后写覆盖先写）
using Conf = std::vector<std::pair<std::string, std::string>>;

struct ConfParseResult {
    Conf kv;
    std::vector<std::string> bad_lines;  // 无法解析的行（供自检报告，不静默丢弃）
};

// 解析行式配置文件：
//   - 忽略空行与 # 注释行
//   - key=value 按第一个 = 切分，键与值均去首尾空白
//   - 值若整体被成对引号包裹则剥离引号
//   - 无 = 或键为空的行记入 bad_lines
ConfParseResult conf_parse_file(const std::string& path);

bool conf_get(const Conf& kv, const std::string& key, std::string& out);
std::string conf_get_or(const Conf& kv, const std::string& key, const std::string& def);
bool conf_has(const Conf& kv, const std::string& key);

// 模块配置寻址（模块自身文件：真实路径，不经 nd_path 前缀化——
// XD_ROOT 前缀只用于设备状态路径；详见 config.cpp 顶部约定）
std::string settings_path();
std::string profile_path(const std::string& name);
std::string perapp_path(const std::string& pkg);
std::vector<std::string> perapp_list();

// 模块目录：优先环境变量 XDD_MODDIR，否则由 /proc/self/exe 推导
std::string module_dir();

}  // namespace xdd
