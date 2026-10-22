#pragma once
#include <string>

namespace xdd {

// host/device 同源核心契约：
// 所有设备路径（/sys、/proc、/data/adb/...）一律经 nd_path() 前缀化。
// host 测试设 XD_ROOT=<模拟设备树>，整体切换到模拟树；
// device 构建 XD_ROOT 为空，直写真实路径。
std::string nd_path(const char* abs_path);
std::string nd_path(const std::string& abs_path);

// 当前是否为 host 测试模式（XD_ROOT 非空）
bool host_mode();

// XD_ROOT 原值（可能为空）
const std::string& xd_root();

}  // namespace xdd
