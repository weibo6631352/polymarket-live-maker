// pmm/ratelimit.hpp — 线程安全 token-bucket 限速 (port of pm_trader/ratelimit.py)
//
// CLOB 全局 per-key 请求上限 (~149 req/s)。做市环的读 (book/mid) 与写 (cancel/place) 共享这一预算,
// 单个共享桶为每个请求节流。acquire 阻塞调用线程直到有 token, 故 N 个并发 poller 自动节流到全局速率。
// 低优先级 (读) 调用保留 reserve 个 token 给高优先级 (cancel/place), 后者永不排在读洪流后面。
#pragma once

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

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

// 按端点独立限速 (Polymarket 官方 per-endpoint 限额: docs.polymarket.com/api-reference/rate-limits)。
// 每个端点一个独立桶 → /book(150/s) 高频拉盘口与 POST /order(200/s) 下单互不挤占。
// clob host 桶(900/s)兜底。之前用单一 149 共享桶把所有端点捆死 = 浪费 ~6× 吞吐, 已弃。
class RateLimiter {
public:
    RateLimiter() : host_(900.0, 1500.0) {}  // clob host: 9000/10s=900/s, burst 1500

    // 按 path+method 分类到对应端点桶并阻塞取 token (+ host 桶兜底)。
    void acquire(const std::string& path, const char* method) {
        const auto [key, rate] = classify(path, method);
        bucket_for(key, rate)->acquire();
        host_.acquire();
    }

    // 诊断: 某端点已发请求数。
    double granted_for(const std::string& key) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = buckets_.find(key);
        return it == buckets_.end() ? 0.0 : it->second->granted();
    }

    // path+method -> (端点桶 key, 每秒上限)。数字全部来自官方 rate-limits 文档。
    static std::pair<std::string, double> classify(const std::string& p, const char* m) {
        const bool write = (m != nullptr) && (std::strcmp(m, "POST") == 0 || std::strcmp(m, "DELETE") == 0);
        const auto has = [&](const char* s) { return p.find(s) != std::string::npos; };
        if (write && has("/cancel-all")) return {"cancel_all", 25.0};
        if (write && has("/orders")) return {"orders_batch", 200.0};  // batch 2000/10s
        if (write && has("/order")) return {"order_write", 200.0};    // 5000/10s burst, 200 持续
        if (has("/data/trades")) return {"data_trades", 50.0};
        if (has("/prices-history")) return {"prices_history", 100.0};
        if (has("/books")) return {"books_batch", 50.0};
        if (has("/book")) return {"book", 150.0};
        if (has("/midpoints")) return {"midpoints_batch", 50.0};
        if (has("/midpoint")) return {"midpoint", 150.0};
        if (has("/prices")) return {"prices_batch", 50.0};
        if (has("/price")) return {"price", 150.0};
        if (has("/sampling")) return {"sampling", 50.0};  // 未单列, 归 clob general, 保守 50
        if (has("/markets")) return {"markets", 30.0};
        return {"clob_misc", 90.0};  // 其它 clob GET 的保守默认
    }

private:
    TokenBucket* bucket_for(const std::string& key, double rate) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = buckets_.find(key);
        if (it == buckets_.end()) {
            // 桶容量 = 1 秒额度的 ~20% burst 余量, 至少 rate。
            it = buckets_.emplace(key, std::make_unique<TokenBucket>(rate, std::max(rate, rate * 1.2))).first;
        }
        return it->second.get();  // unique_ptr 永不 erase → 指针稳定, 锁外 acquire 安全
    }

    TokenBucket host_;
    std::map<std::string, std::unique_ptr<TokenBucket>> buckets_;
    std::mutex mu_;
};

}  // namespace pmm
