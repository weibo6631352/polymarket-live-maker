// pmm/ratelimit.hpp — 线程安全 token-bucket 限速 (port of pm_trader/ratelimit.py)
//
// CLOB 全局 per-key 请求上限 (~149 req/s)。做市环的读 (book/mid) 与写 (cancel/place) 共享这一预算,
// 单个共享桶为每个请求节流。acquire 阻塞调用线程直到有 token, 故 N 个并发 poller 自动节流到全局速率。
// 低优先级 (读) 调用保留 reserve 个 token 给高优先级 (cancel/place), 后者永不排在读洪流后面。
#pragma once

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <thread>

namespace pmm {

class TokenBucket {
public:
    explicit TokenBucket(double rate, std::optional<double> burst = std::nullopt, double reserve = 0.0)
        : rate_(rate),
          capacity_(burst.has_value() ? *burst : std::max(1.0, rate)),
          reserve_(reserve),
          tokens_(capacity_),
          last_(clock::now()) {}

    // 立即取 n 个 token (有则取, 永不阻塞)。低优先级留 reserve 给高优先级写。
    bool try_acquire(double n = 1.0, bool low_priority = false) {
        const double floor = low_priority ? reserve_ : 0.0;
        std::lock_guard<std::mutex> lk(mu_);
        refill_locked();
        if (tokens_ >= n + floor) {
            tokens_ -= n;
            count_locked(n, low_priority);
            return true;
        }
        return false;
    }

    // 阻塞直到取到 n 个 (或 timeout 到期返回 false)。
    bool acquire(double n = 1.0, std::optional<double> timeout = std::nullopt,
                 bool low_priority = false) {
        const double floor = low_priority ? reserve_ : 0.0;
        std::optional<clock::time_point> deadline;
        if (timeout.has_value()) {
            deadline = clock::now() + std::chrono::duration_cast<clock::duration>(dur(*timeout));
        }
        for (;;) {
            double wait = 0.05;
            {
                std::lock_guard<std::mutex> lk(mu_);
                refill_locked();
                if (tokens_ >= n + floor) {
                    tokens_ -= n;
                    count_locked(n, low_priority);
                    return true;
                }
                const double deficit = n + floor - tokens_;
                wait = rate_ > 0.0 ? deficit / rate_ : 0.05;
            }
            if (deadline.has_value()) {
                const double remaining = secs(*deadline - clock::now());
                if (remaining <= 0.0) return false;
                wait = std::min(wait, remaining);
            }
            std::this_thread::sleep_for(dur(std::min(std::max(wait, 0.0), 0.05)));
        }
    }

    double available() {
        std::lock_guard<std::mutex> lk(mu_);
        refill_locked();
        return tokens_;
    }

    [[nodiscard]] double rate() const noexcept { return rate_; }
    [[nodiscard]] double capacity() const noexcept { return capacity_; }
    [[nodiscard]] double reserve() const noexcept { return reserve_; }
    double granted() {
        std::lock_guard<std::mutex> lk(mu_);
        return granted_;
    }
    double granted_low() {
        std::lock_guard<std::mutex> lk(mu_);
        return granted_low_;
    }

private:
    using clock = std::chrono::steady_clock;
    static std::chrono::duration<double> dur(double s) { return std::chrono::duration<double>(s); }
    static double secs(clock::duration d) { return std::chrono::duration<double>(d).count(); }

    void refill_locked() {
        const auto t = clock::now();
        const double elapsed = secs(t - last_);
        if (elapsed > 0.0) {
            tokens_ = std::min(capacity_, tokens_ + elapsed * rate_);
            last_ = t;
        }
    }
    void count_locked(double n, bool low_priority) {
        granted_ += n;
        if (low_priority) granted_low_ += n;
    }

    double rate_;
    double capacity_;
    double reserve_;
    double tokens_;
    clock::time_point last_;
    std::mutex mu_;
    double granted_{0.0};
    double granted_low_{0.0};
};

}  // namespace pmm
