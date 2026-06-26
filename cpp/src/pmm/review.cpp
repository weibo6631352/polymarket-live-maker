// src/pmm/review.cpp — 事后复盘 & 奖励对账实现 (port of pm_trader/review.py)
#include "pmm/review.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>

#include "pmm/db.hpp"
#include "pmm/net/https_pool.hpp"
#include "pmm/round.hpp"

namespace pmm::review {

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

double jnum(const json& j, const char* key, double def = 0.0) {
    auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<double>() : def;
}

std::string jstr(const json& j, const char* key) {
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}

// float(r.get("usdcSize") or r.get("size") or 0.0): 兼容字符串/数字, falsy → 0。
double parse_usdc(const json& r) {
    for (const char* k : {"usdcSize", "size"}) {
        auto it = r.find(k);
        if (it == r.end() || it->is_null()) continue;
        double v = 0.0;
        if (it->is_number()) {
            v = it->get<double>();
        } else if (it->is_string()) {
            try {
                v = std::stod(it->get<std::string>());
            } catch (...) {
                v = 0.0;
            }
        }
        if (v != 0.0) return v;
    }
    return 0.0;
}

}  // namespace

std::vector<json> fetch_actual_rewards(const std::string& wallet, std::optional<long long> start,
                                       std::optional<long long> end, int page, int max_pages) {
    std::vector<json> out;
    if (wallet.empty()) return out;
    net::HttpsPool pool("data-api.polymarket.com", 2, 20000);
    std::set<std::string> seen_tx;
    try {
        for (int i = 0; i < max_pages; ++i) {
            std::string path = "/activity?user=" + wallet + "&type=REWARD&limit=" + std::to_string(page) +
                               "&offset=" + std::to_string(i * page);
            if (start) path += "&start=" + std::to_string(*start);
            if (end) path += "&end=" + std::to_string(*end);
            const net::HttpResponse resp = pool.Get(path);
            if (resp.status < 200 || resp.status >= 300) break;
            json rows;
            try {
                rows = json::parse(resp.body);
            } catch (...) {
                break;
            }
            if (!rows.is_array() || rows.empty()) break;
            int fresh = 0;
            for (const auto& r : rows) {
                const std::string tx = jstr(r, "transactionHash");
                const std::string cond = jstr(r, "conditionId");
                const std::string ts =
                    r.contains("timestamp") ? json(r["timestamp"]).dump() : std::string{};
                const std::string key = tx + "|" + cond + "|" + ts;
                if (seen_tx.count(key) != 0) continue;  // 防 offset 不前进
                seen_tx.insert(key);
                json row;
                row["ts"] = r.contains("timestamp") ? r["timestamp"] : json(nullptr);
                row["condition_id"] = cond;
                row["usdc"] = parse_usdc(r);
                row["tx"] = tx;
                out.push_back(std::move(row));
                ++fresh;
            }
            if (static_cast<int>(rows.size()) < page || fresh == 0) break;
        }
    } catch (...) {
        // best-effort: 返回已拿到的
    }
    return out;
}

std::vector<json> load_events(const std::string& events_dir, std::optional<int> days) {
    std::vector<json> out;
    std::error_code ec;
    if (!fs::exists(events_dir, ec)) return out;
    std::string cutoff;  // YYYYMMDD
    if (days) {
        const auto now = std::chrono::system_clock::now();
        const std::time_t t = std::chrono::system_clock::to_time_t(now) -
                              static_cast<std::time_t>(*days) * 86400;
        std::tm tm{};
        gmtime_r(&t, &tm);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%04d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        cutoff = buf;
    }
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(events_dir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("events-", 0) == 0 && e.path().extension() == ".jsonl") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& p : files) {
        std::string stem = p.stem().string();  // events-YYYYMMDD
        const std::string day = stem.rfind("events-", 0) == 0 ? stem.substr(7) : stem;
        if (!cutoff.empty() && day < cutoff) continue;
        std::ifstream f(p);
        std::string line;
        while (std::getline(f, line)) {
            // trim
            const auto b = line.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) continue;
            const auto e2 = line.find_last_not_of(" \t\r\n");
            line = line.substr(b, e2 - b + 1);
            try {
                out.push_back(json::parse(line));
            } catch (...) {
            }
        }
    }
    return out;
}

json summarize(const std::vector<MakerQuote>& quotes, const std::vector<json>& events,
               const std::vector<json>& actual) {
    // 预估侧, per-market (插入序保留, 与 Python dict 一致)。
    std::vector<json> pools;
    std::map<std::string, std::size_t> idx;
    auto ensure = [&](const std::string& cond, const std::string& slug, const std::string& status) -> json& {
        auto it = idx.find(cond);
        if (it != idx.end()) return pools[it->second];
        json p = {{"condition_id", cond}, {"slug", slug},     {"est_reward", 0.0},
                  {"bleed", 0.0},          {"inv_pnl", 0.0},   {"fills", 0},
                  {"actual_reward", 0.0},  {"n_quotes", 0},    {"status", status}};
        idx[cond] = pools.size();
        pools.push_back(std::move(p));
        return pools.back();
    };

    for (const auto& q : quotes) {
        json& p = ensure(q.market_condition_id, q.market_slug, q.status);
        p["est_reward"] = p["est_reward"].get<double>() + q.accrued_rewards;
        p["bleed"] = p["bleed"].get<double>() + q.realized_bleed;
        p["inv_pnl"] = p["inv_pnl"].get<double>() + q.inventory_pnl;
        p["fills"] = p["fills"].get<int>() + q.fills;
        p["n_quotes"] = p["n_quotes"].get<int>() + 1;
        p["status"] = q.status;  // last seen
    }

    // 实际侧, per-market (首见序累加)。
    std::vector<std::string> actual_order;
    std::map<std::string, double> actual_by_cond;
    for (const auto& a : actual) {
        const std::string cond = jstr(a, "condition_id");
        if (actual_by_cond.find(cond) == actual_by_cond.end()) actual_order.push_back(cond);
        actual_by_cond[cond] += jnum(a, "usdc", 0.0);
    }
    for (const auto& cond : actual_order) {
        json& p = ensure(cond, "", "");
        p["actual_reward"] = p["actual_reward"].get<double>() + actual_by_cond[cond];
    }

    for (auto& p : pools) {
        p["net_est"] = round_to(p["est_reward"].get<double>() + p["inv_pnl"].get<double>(), 4);
        p["est_reward"] = round_to(p["est_reward"].get<double>(), 4);
        p["actual_reward"] = round_to(p["actual_reward"].get<double>(), 4);
        p["bleed"] = round_to(p["bleed"].get<double>(), 4);
        p["inv_pnl"] = round_to(p["inv_pnl"].get<double>(), 4);
    }

    // 决策 + discovery (事件日志)。
    int places = 0;
    std::map<std::string, int> exits_by_reason;
    int discos = 0;
    double safe_sum = 0.0;
    for (const auto& e : events) {
        const std::string kind = jstr(e, "kind");
        if (kind == "place") {
            ++places;
        } else if (kind == "exit") {
            std::string reason = jstr(e, "reason");
            if (reason.empty()) reason = "?";
            exits_by_reason[reason] += 1;
        } else if (kind == "discovery") {
            ++discos;
            safe_sum += jnum(e, "safe", 0.0);
        }
    }
    const double avg_safe = discos != 0 ? round_to(safe_sum / discos, 1) : 0.0;

    double est_total = 0.0, act_total = 0.0, bleed_total = 0.0, inv_pnl_total = 0.0;
    for (const auto& p : pools) {
        est_total += p["est_reward"].get<double>();
        act_total += p["actual_reward"].get<double>();
        bleed_total += p["bleed"].get<double>();
        inv_pnl_total += p["inv_pnl"].get<double>();
    }
    est_total = round_to(est_total, 4);
    act_total = round_to(act_total, 4);
    bleed_total = round_to(bleed_total, 4);
    inv_pnl_total = round_to(inv_pnl_total, 4);

    // per_pool 排序: 降序 (actual+est), 稳定 (插入序定 tie)。
    std::stable_sort(pools.begin(), pools.end(), [](const json& a, const json& b) {
        return (a["actual_reward"].get<double>() + a["est_reward"].get<double>()) >
               (b["actual_reward"].get<double>() + b["est_reward"].get<double>());
    });

    json decisions_exits = json::object();
    for (const auto& [reason, n] : exits_by_reason) decisions_exits[reason] = n;

    json report;
    report["estimated_reward_total"] = est_total;
    report["actual_reward_total"] = act_total;
    report["reconciliation_ratio"] = est_total > 0.0 ? json(round_to(act_total / est_total, 3)) : json(nullptr);
    report["bleed_total"] = bleed_total;
    report["inventory_pnl_total"] = inv_pnl_total;
    report["net_estimated"] = round_to(est_total + inv_pnl_total, 4);
    report["net_actual"] = round_to(act_total + inv_pnl_total, 4);
    report["decisions"] = {{"places", places}, {"exits_by_reason", decisions_exits}};
    report["discovery"] = {{"scans", discos}, {"avg_safe_pools", avg_safe}};
    report["per_pool"] = pools;
    return report;
}

std::string format_report(const json& r) {
    const json& ratio = r["reconciliation_ratio"];
    char buf[256];
    std::string ratio_s;
    if (ratio.is_number()) {
        std::snprintf(buf, sizeof(buf), "%.2fx", ratio.get<double>());
        ratio_s = buf;
    } else {
        ratio_s = "n/a (no estimate)";
    }
    const std::string bar64(64, '=');
    const std::string dash64(64, '-');
    std::string s;
    s += bar64 + "\n";
    s += "MAKER REVIEW — estimate vs ACTUAL reward reconciliation\n";
    s += bar64 + "\n";
    std::snprintf(buf, sizeof(buf), "estimated reward : $%.2f\n", r["estimated_reward_total"].get<double>());
    s += buf;
    std::snprintf(buf, sizeof(buf), "ACTUAL reward    : $%.2f   (actual/est = %s)\n",
                  r["actual_reward_total"].get<double>(), ratio_s.c_str());
    s += buf;
    std::snprintf(buf, sizeof(buf), "adverse bleed    : $%.2f\n", r["bleed_total"].get<double>());
    s += buf;
    std::snprintf(buf, sizeof(buf), "inventory P&L    : $%.2f\n", r["inventory_pnl_total"].get<double>());
    s += buf;
    std::snprintf(buf, sizeof(buf), "net (estimated)  : $%.2f\n", r["net_estimated"].get<double>());
    s += buf;
    std::snprintf(buf, sizeof(buf), "net (ACTUAL)     : $%.2f\n", r["net_actual"].get<double>());
    s += buf;
    s += dash64 + "\n";
    const json& exits = r["decisions"]["exits_by_reason"];
    std::snprintf(buf, sizeof(buf), "decisions: %d placements | exits: %s\n",
                  r["decisions"]["places"].get<int>(), exits.empty() ? "{}" : exits.dump().c_str());
    s += buf;
    std::snprintf(buf, sizeof(buf), "discovery: %d scans | avg %.1f safe pools/scan\n",
                  r["discovery"]["scans"].get<int>(), r["discovery"]["avg_safe_pools"].get<double>());
    s += buf;
    s += dash64 + "\n";
    std::snprintf(buf, sizeof(buf), "%-28s %8s %9s %8s %6s %9s\n", "market", "est$", "actual$", "bleed$",
                  "fills", "status");
    s += buf;
    int shown = 0;
    for (const auto& p : r["per_pool"]) {
        if (shown++ >= 25) break;
        std::string name = jstr(p, "slug");
        if (name.empty()) name = jstr(p, "condition_id");
        if (name.size() > 27) name = name.substr(0, 27);
        std::snprintf(buf, sizeof(buf), "%-28s %8.2f %9.2f %8.2f %6d %9s\n", name.c_str(),
                      p["est_reward"].get<double>(), p["actual_reward"].get<double>(),
                      p["bleed"].get<double>(), p["fills"].get<int>(), jstr(p, "status").c_str());
        s += buf;
    }
    if (ratio.is_number() && ratio.get<double>() < 0.6) {
        s += dash64 + "\n";
        std::snprintf(buf, sizeof(buf),
                      "⚠ actual is only %.0f%% of estimate — share is being over-estimated "
                      "(book snapshot vs Polymarket's full-day settlement).",
                      ratio.get<double>() * 100.0);
        s += buf;
    } else if (!s.empty() && s.back() == '\n') {
        s.pop_back();  // 去掉最后换行 (Python "\n".join 无尾换行)
    }
    return s;
}

json run_review(const std::string& state_dir, const std::string& wallet, int days) {
    std::vector<MakerQuote> quotes;
    try {
        Database db(state_dir);
        try {
            quotes = db.get_all_maker_quotes();
        } catch (...) {
            quotes.clear();
        }
        db.close();
    } catch (...) {
    }
    const std::vector<json> events = load_events((fs::path(state_dir) / "events").string(), days);
    std::vector<json> actual;
    if (!wallet.empty()) {
        const auto now = std::chrono::system_clock::now();
        const long long start = static_cast<long long>(std::chrono::system_clock::to_time_t(now)) -
                                static_cast<long long>(days) * 86400;
        actual = fetch_actual_rewards(wallet, start);
    }
    return summarize(quotes, events, actual);
}

}  // namespace pmm::review
