// src/pmm/inflight.cpp — 在途成交记账实现 (见 inflight.hpp)
#include "pmm/inflight.hpp"

#include <cmath>
#include <set>

namespace pmm {

void InFlightLedger::on_fill(const std::string& token, double signed_size, double price, double now_mono) {
    if (std::abs(signed_size) < kEps || token.empty()) return;
    by_token_[token].push_back(Entry{signed_size, price, now_mono});
}

void InFlightLedger::retire_oldest(const std::string& token, double moved) {
    auto it = by_token_.find(token);
    if (it == by_token_.end()) return;
    auto& dq = it->second;
    double remaining = moved;  // 签名: 链上正向移动(涨)→ 退役最旧的买(正); 负向 → 退役最旧的卖(负)
    while (!dq.empty() && std::abs(remaining) >= kEps) {
        Entry& e = dq.front();
        if ((remaining > 0.0) != (e.signed_size > 0.0)) break;  // 方向不符 → 停 (不乱退役反向项)
        if (std::abs(e.signed_size) <= std::abs(remaining) + kEps) {
            remaining -= e.signed_size;  // 整笔被链上吸收
            dq.pop_front();
        } else {
            e.signed_size -= remaining;  // 部分吸收, 余量仍在途
            remaining = 0.0;
        }
    }
}

void InFlightLedger::reconcile(const std::map<std::string, double>& chain_positions, double now_mono) {
    std::set<std::string> tokens;
    for (const auto& [t, dq] : by_token_) tokens.insert(t);
    for (const auto& [t, v] : prev_chain_) tokens.insert(t);
    for (const auto& [t, v] : chain_positions) tokens.insert(t);

    for (const auto& t : tokens) {
        double observed = 0.0;
        if (auto c = chain_positions.find(t); c != chain_positions.end()) observed = c->second;
        double prev = 0.0;
        if (auto p = prev_chain_.find(t); p != prev_chain_.end()) prev = p->second;
        retire_oldest(t, observed - prev);  // 链上移动 = 结算了的在途量 → 退役
        // 剪枝: 位置归零且无在途 → 不再跟踪 (防 prev_chain_ 无界增长)
        if (std::abs(observed) < kEps && by_token_.find(t) == by_token_.end()) {
            prev_chain_.erase(t);
        } else {
            prev_chain_[t] = observed;
        }
    }

    // TTL 老化兜底: 最旧项 (deque 按时序追加, front 最旧) 超龄强制退役。
    for (auto it = by_token_.begin(); it != by_token_.end();) {
        auto& dq = it->second;
        while (!dq.empty() && now_mono - dq.front().acked_mono > kTtlSeconds) dq.pop_front();
        if (dq.empty())
            it = by_token_.erase(it);
        else
            ++it;
    }
}

double InFlightLedger::believed_delta(const std::string& token) const {
    auto it = by_token_.find(token);
    if (it == by_token_.end()) return 0.0;
    double d = 0.0;
    for (const auto& e : it->second) d += e.signed_size;
    return d;
}

double InFlightLedger::net_cost() const {
    double c = 0.0;
    for (const auto& [t, dq] : by_token_)
        for (const auto& e : dq) c += e.signed_size * e.price;
    return c;
}

std::map<std::string, double> InFlightLedger::believed_positions(
    const std::map<std::string, double>& chain_positions) const {
    std::map<std::string, double> out = chain_positions;
    for (const auto& [t, dq] : by_token_) {
        double d = 0.0;
        for (const auto& e : dq) d += e.signed_size;
        out[t] += d;
    }
    return out;
}

std::size_t InFlightLedger::size() const {
    std::size_t n = 0;
    for (const auto& [t, dq] : by_token_) n += dq.size();
    return n;
}

}  // namespace pmm
