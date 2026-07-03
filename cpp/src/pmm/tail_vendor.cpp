// tail_vendor.cpp — 决策核实现 (纯函数, 无 IO)。见 tail_vendor.hpp 头注释。
#include "pmm/tail_vendor.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>


namespace pmm::tail {
namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool contains_any(const std::string& s, std::initializer_list<const char*> words) {
    for (const char* w : words)
        if (s.find(w) != std::string::npos) return true;
    return false;
}

// endDate "2026-07-04T16:00:00Z" -> unix; 失败 0。
double parse_iso(const std::string& iso) {
    std::tm tm{};
    if (iso.size() < 19) return 0;
    if (sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour,
               &tm.tm_min, &tm.tm_sec) != 6)
        return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return static_cast<double>(timegm(&tm));
}

std::string coin_of(const std::string& s) {
    // DOGE 排除: 校准样本不足 (n=116, uncalibratable)。
    if (contains_any(s, {"bitcoin", "btc"})) return "btc";
    if (contains_any(s, {"ethereum", "eth"})) return "eth";
    if (contains_any(s, {"solana", "sol"})) return "sol";
    if (contains_any(s, {"xrp"})) return "xrp";
    return "";
}

double round_tick(double px, double tick) { return std::round(px / tick) * tick; }

}  // namespace

std::optional<Candidate> parse_candidate(const nlohmann::json& m, double now_unix) {
    const std::string slug = lower(m.value("slug", ""));
    const std::string q = lower(m.value("question", ""));
    const std::string sq = slug + " " + q;

    const std::string coin = coin_of(sq);
    if (coin.empty()) return std::nullopt;
    // 只做上行尾: 需要向上方向词, 且不含向下方向词。这同时天然排除 touch 家族
    // ("reach/hit/dip" 无上行词) 与微市场 ("up or down" 无上行词) — 无需单列检查。
    if (contains_any(sq, {"below", "less than", "or-lower", "or lower", "dip"})) return std::nullopt;
    if (!contains_any(sq, {"above", "greater than", "-greater-", "or-higher", "or higher"}))
        return std::nullopt;

    // token 结构安全检查: outcomes 必须是 ["Yes","No"] (NO = tokens[1])。
    try {
        const auto outcomes_raw = m.value("outcomes", nlohmann::json());
        nlohmann::json outcomes = outcomes_raw;
        if (outcomes.is_string()) outcomes = nlohmann::json::parse(outcomes.get<std::string>());
        if (!outcomes.is_array() || outcomes.size() != 2 ||
            lower(outcomes[0].get<std::string>()) != "yes" ||
            lower(outcomes[1].get<std::string>()) != "no")
            return std::nullopt;
        nlohmann::json toks = m.value("clobTokenIds", nlohmann::json());
        if (toks.is_string()) toks = nlohmann::json::parse(toks.get<std::string>());
        if (!toks.is_array() || toks.size() != 2) return std::nullopt;

        Candidate c;
        c.cond = m.value("conditionId", m.value("condition_id", ""));
        c.slug = m.value("slug", "");
        c.yes_token = toks[0].get<std::string>();
        c.no_token = toks[1].get<std::string>();
        c.coin = coin;
        c.yes_bid = m.contains("bestBid") && m["bestBid"].is_number() ? m["bestBid"].get<double>()
                    : m.contains("bestBid") ? std::atof(m.value("bestBid", "0").c_str()) : 0.0;
        c.yes_ask = m.contains("bestAsk") && m["bestAsk"].is_number() ? m["bestAsk"].get<double>()
                    : m.contains("bestAsk") ? std::atof(m.value("bestAsk", "1").c_str()) : 1.0;
        const double end = parse_iso(m.value("endDate", ""));
        if (end <= 0 || c.cond.empty() || c.no_token.empty()) return std::nullopt;
        c.days_left = (end - now_unix) / 86400.0;
        return c;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<Quote> decide(const Candidate& c, const Config& cfg, double deployed_coin,
                            double deployed_total) {
    if (c.days_left < cfg.days_min || c.days_left > cfg.days_max) return std::nullopt;
    if (c.yes_ask <= 0 || c.yes_ask >= 1) return std::nullopt;
    // 卖价 = 压最优卖一 1 tick, 但不低于 floor; 必须仍在校准带内且不越过买一 (绝不当 taker)。
    double sell_yes = std::max(cfg.floor_yes, c.yes_ask - cfg.tick);
    sell_yes = round_tick(sell_yes, cfg.tick);
    if (sell_yes < cfg.yes_min || sell_yes > cfg.yes_max) return std::nullopt;
    if (sell_yes <= c.yes_bid + 1e-9) return std::nullopt;

    const double no_price = round_tick(1.0 - sell_yes, cfg.tick);
    double size = std::floor(cfg.per_order_usd / no_price);
    if (size < cfg.min_shares) return std::nullopt;
    const double notional = size * no_price;
    if (deployed_coin + notional > cfg.per_coin_usd + 1e-9) return std::nullopt;
    if (deployed_total + notional > cfg.total_usd + 1e-9) return std::nullopt;

    return Quote{c.no_token, no_price, size, c.slug};
}

bool should_pull(const Candidate& c, const Config& cfg) {
    const double mid = (c.yes_bid + c.yes_ask) / 2.0;
    if (mid >= cfg.yes_exit) return true;                      // 行情逼近障碍 -> 撤
    if (c.days_left < cfg.days_min * 0.5) return true;         // 临期 -> 撤 (最后半天不接新逆选)
    if (c.yes_ask - c.yes_bid > 0.20) return true;             // 书面崩坏 -> 撤
    return false;
}

std::vector<Action> plan(const std::map<std::string, Candidate>& cands,
                         const std::vector<OpenOrder>& open,
                         const std::map<std::string, double>& held_coin, double held_total,
                         const Config& cfg) {
    std::vector<Action> out;
    // ---- 撤单侧: 离场/pull/被压价 ----
    std::map<std::string, const OpenOrder*> keep;  // 留在场上的单
    for (const auto& o : open) {
        const auto ic = cands.find(o.no_token);
        const char* why = nullptr;
        if (ic == cands.end()) why = "left_window";
        else if (should_pull(ic->second, cfg)) why = "pull_signal";
        else if (ic->second.yes_ask < (1.0 - o.no_price) - cfg.tick - 1e-9) why = "outbid";
        if (why != nullptr)
            out.push_back({Action::Kind::kCancel, o.no_token, 0, 0, why, o.note});
        else
            keep[o.no_token] = &o;
    }
    // ---- 报单侧: 轮内累计 (resting=留场单 + held; 撤掉的预算即时释放) ----
    double total = held_total;
    std::map<std::string, double> coin = held_coin;
    for (const auto& [tok, po] : keep) {
        total += po->no_price * po->size;
        if (!po->coin.empty()) coin[po->coin] += po->no_price * po->size;
    }
    int n_orders = static_cast<int>(keep.size());
    for (const auto& [tok, c] : cands) {
        if (keep.count(tok) != 0U) continue;
        if (n_orders >= cfg.max_orders) break;
        const auto q = decide(c, cfg, coin[c.coin], total);
        if (!q) continue;
        out.push_back({Action::Kind::kPlace, q->no_token, q->no_price, q->size, "", q->note});
        const double notional = q->no_price * q->size;
        total += notional;
        coin[c.coin] += notional;
        ++n_orders;
    }
    return out;
}

}  // namespace pmm::tail
