// pmm/infra/process/fd_guard.hpp — POSIX fd RAII (vendored from sports-trader-cpp)
#pragma once

#include <unistd.h>

namespace pmm::infra::process {

// fd_ == -1 ⟺ 不持有; fd_ >= 0 ⟺ 持有 (析构 ::close)。
class FdGuard {
public:
    FdGuard() noexcept = default;
    explicit FdGuard(int fd) noexcept : fd_(fd) {}
    ~FdGuard() noexcept { do_close(); }
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
    FdGuard(FdGuard&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    FdGuard& operator=(FdGuard&& o) noexcept {
        if (this != &o) {
            do_close();
            fd_ = o.fd_;
            o.fd_ = -1;
        }
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    [[nodiscard]] int get() const noexcept { return fd_; }
    void reset(int new_fd = -1) noexcept {
        do_close();
        fd_ = new_fd;
    }
    bool close_now() noexcept {
        if (fd_ < 0) return true;
        const int ret = ::close(fd_);
        fd_ = -1;
        return ret == 0;
    }

private:
    void do_close() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    int fd_{-1};
};

}  // namespace pmm::infra::process
