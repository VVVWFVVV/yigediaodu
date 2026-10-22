#pragma once
#include <string>
#include <utility>
#include <vector>

namespace xdd {

// 运行时目录（均经 nd_path 前缀化）
std::string runtime_dir();  // /data/adb/WY925976742

// 当前生效状态（单文件多行 key=value）
struct CurrentState {
    std::string pkg;
    std::string profile;
    std::string mode;  // fg / bg
    long since = 0;    // epoch 秒
};
void write_current(const CurrentState& st);
bool read_current(CurrentState& out);
void clear_current();

// 能力生效状态：capability 文件中的 key=value（ok / degraded / off）
void set_capability(const std::string& name, const std::string& status);
std::string get_capability(const std::string& name);

// 心跳（监督者以 mtime 判活）
void write_heartbeat();

// live 快照（单文件多行 key=value，WebUI 只读展示）
void write_live(const std::vector<std::pair<std::string, std::string>>& kv);

}  // namespace xdd
