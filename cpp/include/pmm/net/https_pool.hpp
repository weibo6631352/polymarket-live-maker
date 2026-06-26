// pmm/net/https_pool.hpp — 线程安全的 PersistentHttps 连接池 (一个 host)。
//
// PersistentHttps 单连接非线程安全 (sports-trader 里由单个 poller 独占)。本工程的轮询从多线程发起
// (poll 环 + discovery + N 个 resync worker), 需要并发 + keep-alive。本池维护最多 max 条热连接:
// Get 借出一条 (空闲则复用, 否则新建到上限, 满则等待) → 用完归还。复用 TLS 握手, 又支持并发压满 149/s。
#pragma once

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "pmm/net/persistent_https.hpp"

namespace pmm::net {

struct HttpResponse {
    int status{0};       // HTTP 状态码 (0 = 连接/超时失败)
    std::string body;
};

class HttpsPool {
public:
    HttpsPool(std::string host, std::size_t max_conns, int timeout_ms = 4000)
        : host_(std::move(host)), max_(max_conns == 0 ? 1 : max_conns), timeout_ms_(timeout_ms) {}

    // 借一条连接发 GET path, 用完归还。返回 {status, body}。
    // 无论 body/status 处理是否抛 (如 OOM), 连接都必归还 → 不泄漏槽位 (否则累积到 max_ 会死锁全队)。
    HttpResponse Get(const std::string& path) {
        std::unique_ptr<PersistentHttps> conn = checkout();
        HttpResponse r;
        try {
            r.body = conn->Get(path);  // PersistentHttps::Get 是 noexcept
            r.status = conn->last_status();
        } catch (...) {
            checkin(std::move(conn));
            throw;
        }
        checkin(std::move(conn));
        return r;
    }

private:
    std::unique_ptr<PersistentHttps> checkout() {
        std::unique_lock<std::mutex> lk(mu_);
        for (;;) {
            if (!idle_.empty()) {
                std::unique_ptr<PersistentHttps> c = std::move(idle_.back());
                idle_.pop_back();
                ++outstanding_;
                return c;
            }
            if (outstanding_ < max_) {
                ++outstanding_;
                lk.unlock();
                try {
                    return std::make_unique<PersistentHttps>(host_, timeout_ms_);
                } catch (...) {
                    // 构造失败 (如 OOM): 回滚 outstanding_ 并唤醒等待者, 再抛 (否则永久占一个槽 → 死锁)。
                    lk.lock();
                    --outstanding_;
                    lk.unlock();
                    cv_.notify_one();
                    throw;
                }
            }
            cv_.wait(lk);  // 已达上限, 等一条归还
        }
    }

    void checkin(std::unique_ptr<PersistentHttps> conn) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            idle_.push_back(std::move(conn));
            --outstanding_;
        }
        cv_.notify_one();
    }

    std::string host_;
    std::size_t max_;
    int timeout_ms_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::unique_ptr<PersistentHttps>> idle_;
    std::size_t outstanding_{0};
};

}  // namespace pmm::net
