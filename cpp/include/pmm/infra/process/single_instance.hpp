// pmm/infra/process/single_instance.hpp — 防多开 PID+flock RAII (移植自 sports-trader-cpp, 单路径简化版)
//
// 两个 live-maker 进程绝不能同时跑同一个钱包 (重复下单 / 账本打架)。构造 acquire (flock LOCK_NB),
// 失败抛 SingleInstanceLockFailure。析构 close fd → flock 自动释放。SIGKILL 后 kernel 释放, PID 文件
// 留尾, 下次启动 flock 抢成功后覆盖 (不影响正确性)。残留锁 1s 内自动回收。
#pragma once

#ifndef __linux__
#  ifndef __APPLE__
#    error "SingleInstanceLock only supports Linux and macOS (POSIX flock)"
#  endif
#endif

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

#include "pmm/infra/process/fd_guard.hpp"

namespace pmm::infra::process {

struct SingleInstanceLockFailure : std::runtime_error {
    long long existing_pid{0};
    explicit SingleInstanceLockFailure(std::string msg, long long pid)
        : std::runtime_error(std::move(msg)), existing_pid(pid) {}
};

class SingleInstanceLock {
public:
    // 构造即 acquire pid_path 的独占锁。失败抛 SingleInstanceLockFailure。
    explicit SingleInstanceLock(const std::string& pid_path) { acquire(pid_path); }
    ~SingleInstanceLock() = default;  // FdGuard 析构 close → flock 释放
    SingleInstanceLock(const SingleInstanceLock&) = delete;
    SingleInstanceLock& operator=(const SingleInstanceLock&) = delete;

    [[nodiscard]] const std::string& pid_path() const noexcept { return pid_path_; }

private:
    static bool mkdir_p(const std::string& dir) noexcept {
        if (::mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST) return true;
        const auto slash = dir.rfind('/');
        if (slash == std::string::npos || slash == 0) return false;
        const std::string parent = dir.substr(0, slash);
        if (::mkdir(parent.c_str(), 0755) != 0 && errno != EEXIST) return false;
        return ::mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST;
    }

    static long long read_pid(int fd) noexcept {
        if (::lseek(fd, 0, SEEK_SET) == static_cast<off_t>(-1)) return 0;
        char buf[64]{};
        const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
        if (n <= 0) return 0;
        return std::atoll(buf);
    }

    void acquire(const std::string& path) {
        pid_path_ = path;
        const auto slash = pid_path_.rfind('/');
        if (slash != std::string::npos && slash != 0) {
            if (!mkdir_p(pid_path_.substr(0, slash))) {
                throw SingleInstanceLockFailure("cannot create pid dir for " + pid_path_, 0);
            }
        }
        fd_.reset(::open(pid_path_.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644));
        if (!fd_.valid()) {
            throw SingleInstanceLockFailure("open(" + pid_path_ + ") failed", 0);
        }
        if (::flock(fd_.get(), LOCK_EX | LOCK_NB) != 0) {
            if (errno != EWOULDBLOCK) {
                throw SingleInstanceLockFailure("flock failed errno=" + std::to_string(errno), 0);
            }
            // 残留锁自动回收: 持有者 SIGKILL 后 D 态线程可能短暂未释 → 重试 1s。
            bool acquired = false;
            for (int i = 0; i < 5; ++i) {
                ::usleep(200000);
                if (::flock(fd_.get(), LOCK_EX | LOCK_NB) == 0) {
                    acquired = true;
                    break;
                }
            }
            if (!acquired) {
                const long long pid = read_pid(fd_.get());
                fd_.close_now();
                throw SingleInstanceLockFailure(
                    "another live-maker is already running (pid=" + std::to_string(pid) + ")", pid);
            }
        }
        // 写入自己的 PID (诊断用)。
        ::ftruncate(fd_.get(), 0);
        ::lseek(fd_.get(), 0, SEEK_SET);
        char buf[32];
        const int len = std::snprintf(buf, sizeof(buf), "%lld\n", static_cast<long long>(::getpid()));
        if (len > 0) {
            const ssize_t w = ::write(fd_.get(), buf, static_cast<std::size_t>(len));
            (void)w;
        }
        ::fsync(fd_.get());
    }

    FdGuard fd_;
    std::string pid_path_;
};

}  // namespace pmm::infra::process
