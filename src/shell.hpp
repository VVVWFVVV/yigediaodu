#pragma once
#include <string>

namespace xdd {

struct ShellResult {
    bool ok = false;         // 正常退出且退出码为 0
    bool timed_out = false;  // 超时被杀
    int exit_code = -1;
    std::string out;         // stdout 内容
};

// 执行外部命令（经 /bin/sh -c）并捕获 stdout；
// timeout_ms 超时后 kill 子进程并标记 timed_out —— 杜绝 dumpsys 类命令悬挂
ShellResult shell_run(const std::string& cmd, int timeout_ms);

}  // namespace xdd
