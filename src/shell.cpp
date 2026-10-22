#include "shell.hpp"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

#include "log.hpp"

namespace xdd {

ShellResult shell_run(const std::string& cmd, int timeout_ms) {
    ShellResult r;

    int fds[2];
    if (::pipe(fds) != 0) {
        log_line(LogLevel::Warn, std::string("shell_run pipe 失败: ") + std::strerror(errno));
        return r;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        log_line(LogLevel::Warn, std::string("shell_run fork 失败: ") + std::strerror(errno));
        ::close(fds[0]);
        ::close(fds[1]);
        return r;
    }

    if (pid == 0) {
        // 子进程：stdout 进管道，stderr 丢弃
        ::close(fds[0]);
        ::dup2(fds[1], STDOUT_FILENO);
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
        ::close(fds[1]);
        ::execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
        ::_exit(127);
    }

    // 父进程：非阻塞读 + poll 超时
    ::close(fds[1]);
    ::fcntl(fds[0], F_SETFL, O_NONBLOCK);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    char buf[4096];
    for (;;) {
        int remain = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                         deadline - std::chrono::steady_clock::now())
                         .count();
        if (remain <= 0) {
            r.timed_out = true;
            ::kill(pid, SIGKILL);
            break;
        }
        struct pollfd pfd;
        pfd.fd = fds[0];
        pfd.events = POLLIN;
        int pr = ::poll(&pfd, 1, remain);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) {
            r.timed_out = true;
            ::kill(pid, SIGKILL);
            break;
        }
        ssize_t n = ::read(fds[0], buf, sizeof(buf));
        if (n > 0) {
            r.out.append(buf, (size_t)n);
            continue;
        }
        if (n == 0) break;  // EOF：子进程结束且管道关闭
        if (errno == EINTR || errno == EAGAIN) continue;
        break;
    }

    int status = 0;
    ::waitpid(pid, &status, 0);
    ::close(fds[0]);

    if (r.timed_out) {
        log_line(LogLevel::Warn, "shell_run 超时已杀: " + cmd);
        return r;
    }
    r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    r.ok = (r.exit_code == 0);
    return r;
}

}  // namespace xdd
