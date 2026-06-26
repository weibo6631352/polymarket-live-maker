// src/pmm/maker_sim.cpp — 做市奖励纸面回测实现 (port of pm_trader/maker_sim.py)
#include "pmm/maker_sim.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

#include "pmm/models.hpp"
#include "pmm/round.hpp"

namespace pmm::maker_sim {

using nlohmann::json;

namespace {
// 模仿 Python f"eff_{eff}" (str(float)): 0.0→"0.0", 0.9→"0.9", 0.95→"0.95"。
std::string eff_key(double eff) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", eff);
    std::string s(buf);
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos) s += ".0";
    return "eff_" + s;
}
}  // namespace

json SimResult::to_json() const {
    return {{"steps", steps},
            {"reward_income", round_to(reward_income, 4)},
            {"adverse_bleed", round_to(adverse_bleed, 4)},
            {"unwind_cost", round_to(unwind_cost, 4)},
            {"net", round_to(net, 4)},
            {"pickoffs", pickoffs},
            {"pickoff_rate", round_to(pickoff_rate, 4)},
            {"capital", round_to(capital, 2)},
            {"net_annualized_pct", round_to(net_annualized_pct, 1)}};
}

SimResult simulate_pool(const std::vector<double>& price_path, const SimParams& p) {
    if (!(p.cancel_efficiency >= 0.0 && p.cancel_efficiency <= 1.0)) {
        throw std::invalid_argument("cancel_efficiency must be in [0, 1]");
    }
    if (p.requote_downtime_s < 0.0) throw std::invalid_argument("requote_downtime_s must be >= 0");
    if (p.unwind_cost_ticks < 0.0) throw std::invalid_argument("unwind_cost_ticks must be >= 0");

    const int n_steps = std::max<int>(0, static_cast<int>(price_path.size()) - 1);
    const double half_spread_c = p.tick * 100.0;     // 报价距 mid 一跳
    const double fill_frac = 1.0 - p.cancel_efficiency;  // P(真被成交) — colocation 撤单
    const double unwind_per_fill = p.min_size * p.unwind_cost_ticks * p.tick;
    double bleed_total = 0.0;
    double unwind_total = 0.0;
    int pickoffs = 0;
    for (std::size_t i = 1; i < price_path.size(); ++i) {
        // 注: 不传 cancel_efficiency (默认 0 → raw 出血), 再手动乘 fill_frac (与 Python 一致)。
        const double raw = orderbook::adverse_bleed(p.min_size, half_spread_c, price_path[i - 1], price_path[i]);
        if (raw > 0.0) {  // mid 穿过报价 → 陈旧侧被挑选
            ++pickoffs;
            bleed_total += raw * fill_frac;
            unwind_total += unwind_per_fill * fill_frac;
        }
    }
    // 奖励只在两边在 band 内时累积; 每次被挑选有 downtime (单边持仓直到重新对冲)。
    const double gross_seconds = p.dt_seconds * n_steps;
    const double downtime = std::min(gross_seconds, pickoffs * p.requote_downtime_s);
    const double reward_income = orderbook::reward_accrual(p.share, p.daily, gross_seconds - downtime);
    const double net = reward_income - bleed_total - unwind_total;
    const double capital = std::max(p.min_size, 1e-9);  // 两边 min_size 锁 ≈ min_size 美元
    const double horizon_seconds = std::max(gross_seconds, 1e-9);
    const double net_per_day = net * SECONDS_PER_DAY / horizon_seconds;
    const double net_ann = net_per_day * 365.0 / capital * 100.0;

    SimResult r;
    r.steps = n_steps;
    r.reward_income = reward_income;
    r.adverse_bleed = bleed_total;
    r.unwind_cost = unwind_total;
    r.net = net;
    r.pickoffs = pickoffs;
    r.pickoff_rate = n_steps != 0 ? static_cast<double>(pickoffs) / n_steps : 0.0;
    r.capital = capital;
    r.net_annualized_pct = net_ann;
    return r;
}

std::optional<double> live_pool_share(rewards::RewardsClient& client, const rewards::RewardConfig& pool) {
    json raw;
    try {
        raw = client.book(pool.token);
    } catch (...) {
        return std::nullopt;
    }
    OrderBook book;
    if (auto it = raw.find("bids"); it != raw.end() && it->is_array()) {
        for (const auto& b : *it) {
            if (b.contains("price") && b.contains("size")) {
                book.bids.push_back({std::stod(b["price"].get<std::string>()),
                                     std::stod(b["size"].get<std::string>())});
            }
        }
    }
    if (auto it = raw.find("asks"); it != raw.end() && it->is_array()) {
        for (const auto& a : *it) {
            if (a.contains("price") && a.contains("size")) {
                book.asks.push_back({std::stod(a["price"].get<std::string>()),
                                     std::stod(a["size"].get<std::string>())});
            }
        }
    }
    if (book.bids.empty() || book.asks.empty()) return std::nullopt;
    double best_bid = book.bids.front().price;
    double best_ask = book.asks.front().price;
    for (const auto& b : book.bids) best_bid = std::max(best_bid, b.price);
    for (const auto& a : book.asks) best_ask = std::min(best_ask, a.price);
    const double mid = (best_bid + best_ask) / 2.0;
    const double qmin = orderbook::book_inband_qmin(book, mid, pool.max_spread);
    return orderbook::maker_reward_share(pool.min_size, pool.tick * 100.0, pool.max_spread, qmin);
}

double history_dt_seconds(const std::vector<orderbook::PricePoint>& history) {
    std::vector<double> deltas;
    for (std::size_t i = 1; i < history.size(); ++i) {
        if (history[i].t > history[i - 1].t) deltas.push_back(history[i].t - history[i - 1].t);
    }
    if (deltas.empty()) return 3600.0;
    std::sort(deltas.begin(), deltas.end());
    return deltas[deltas.size() / 2];
}

std::vector<double> path_from_history(const std::vector<orderbook::PricePoint>& history) {
    std::vector<double> path;
    path.reserve(history.size());
    for (const auto& pt : history) path.push_back(pt.p);
    return path;
}

json run_experiment(rewards::RewardsClient& client, const ExperimentParams& ep) {
    std::vector<rewards::RewardConfig> pools;
    for (const auto& m : client.sampling_markets()) {
        if (auto pc = rewards::parse_rewards(m); pc && pc->daily >= ep.min_daily) pools.push_back(*pc);
    }
    std::sort(pools.begin(), pools.end(),
              [](const rewards::RewardConfig& a, const rewards::RewardConfig& b) { return a.daily > b.daily; });
    if (static_cast<int>(pools.size()) > std::max(1, ep.top)) {
        pools.resize(static_cast<std::size_t>(std::max(1, ep.top)));
    }

    struct Agg {
        double reward{0.0}, bleed{0.0}, unwind{0.0}, net{0.0};
    };
    std::vector<Agg> agg(ep.cancel_efficiencies.size());
    json per_pool = json::array();
    for (const auto& p : pools) {
        std::vector<orderbook::PricePoint> history;
        try {
            history = client.prices_history(p.token, "max", ep.fidelity);
        } catch (...) {
            continue;
        }
        const std::vector<double> path = path_from_history(history);
        if (path.size() < 10) continue;
        double pool_share = ep.share;
        if (ep.use_scanned_share) {
            if (auto measured = live_pool_share(client, p)) pool_share = *measured;
        }
        const double dt = history_dt_seconds(history);
        json sims = json::object();
        for (std::size_t k = 0; k < ep.cancel_efficiencies.size(); ++k) {
            const double eff = ep.cancel_efficiencies[k];
            SimParams sp;
            sp.daily = p.daily;
            sp.share = pool_share;
            sp.tick = p.tick;
            sp.min_size = p.min_size;
            sp.dt_seconds = dt;
            sp.cancel_efficiency = eff;
            sp.requote_downtime_s = ep.requote_downtime_s;
            sp.unwind_cost_ticks = ep.unwind_cost_ticks;
            const SimResult r = simulate_pool(path, sp);
            sims[eff_key(eff)] = r.to_json();
            agg[k].reward += r.reward_income;
            agg[k].bleed += r.adverse_bleed;
            agg[k].unwind += r.unwind_cost;
            agg[k].net += r.net;
        }
        per_pool.push_back({{"question", p.question},
                            {"daily", round_to(p.daily, 2)},
                            {"min_size", p.min_size},
                            {"share_used", round_to(pool_share, 4)},
                            {"dt_seconds", dt},
                            {"path_points", path.size()},
                            {"sims", sims}});
    }

    json aggregate = json::object();
    for (std::size_t k = 0; k < ep.cancel_efficiencies.size(); ++k) {
        aggregate[eff_key(ep.cancel_efficiencies[k])] = {
            {"reward_income", round_to(agg[k].reward, 2)},
            {"adverse_bleed", round_to(agg[k].bleed, 2)},
            {"unwind_cost", round_to(agg[k].unwind, 2)},
            {"net", round_to(agg[k].net, 2)}};
    }
    return {{"params",
             {{"min_daily", ep.min_daily},
              {"top", ep.top},
              {"share", ep.share},
              {"cancel_efficiencies", ep.cancel_efficiencies},
              {"use_scanned_share", ep.use_scanned_share},
              {"fidelity", ep.fidelity},
              {"requote_downtime_s", ep.requote_downtime_s},
              {"unwind_cost_ticks", ep.unwind_cost_ticks}}},
            {"pools_simulated", per_pool.size()},
            {"aggregate", aggregate},
            {"pools", per_pool}};
}

}  // namespace pmm::maker_sim
