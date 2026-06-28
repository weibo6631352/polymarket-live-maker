// src/pmm/rewards.cpp — 流动性奖励池扫描器实现 (port of pm_trader/rewards.py)
#include "pmm/rewards.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "pmm/jsonutil.hpp"
#include "pmm/models.hpp"
#include "pmm/orderbook.hpp"  // binding_qmin (官方绑定 Qmin 折叠)
#include "pmm/strutil.hpp"

namespace pmm::rewards {

namespace ju = pmm::jsonutil;
using nlohmann::json;

namespace {

constexpr char kClobHost[] = "clob.polymarket.com";

// Python round(x, n) / round(x): round-half-to-even (FE_TONEAREST 默认)。
double roundn(double x, int n) {
    double f = 1.0;
    for (int i = 0; i < n; ++i) f *= 10.0;
    return std::nearbyint(x * f) / f;
}
double round_int(double x) { return std::nearbyint(x); }

std::optional<double> best_price(const json& levels, bool is_bid) {
    bool any = false;
    double acc = 0.0;
    if (levels.is_array()) {
        for (const auto& lvl : levels) {
            const json* p = ju::find(lvl, "price");
            if (p == nullptr) continue;
            const double price = ju::to_double(*p);
            if (!any) {
                acc = price;
                any = true;
            } else {
                acc = is_bid ? std::max(acc, price) : std::min(acc, price);
            }
        }
    }
    if (!any) return std::nullopt;
    return acc;
}

// 对齐 Python `float(d.get(key, base) or base)`: 缺失/null/falsy → base; truthy 非数值 → nullopt(丢池)。
std::optional<double> py_float_or(const json& obj, const char* key, double base) {
    const json* v = ju::find(obj, key);
    if (v == nullptr || !ju::truthy(*v)) return base;
    if (v->is_number()) return v->get<double>();
    if (v->is_string()) {
        const std::string s = v->get<std::string>();
        try {
            std::size_t pos = 0;
            const double d = std::stod(s, &pos);  // 跳过前导空白
            while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
            if (pos != s.size()) return std::nullopt;  // "3abc" → Python float() 抛异常
            return d;
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;  // truthy 非数非串(array/obj) → float() 抛异常
}

}  // namespace

std::optional<RewardConfig> parse_rewards(const json& market) {
    const json* rw_ptr = ju::find(market, "rewards");
    const json rw = (rw_ptr != nullptr && rw_ptr->is_object()) ? *rw_ptr : json::object();

    double daily = 0.0;
    if (const json* rates = ju::find(rw, "rates")) {
        if (rates->is_array()) {
            for (const auto& rt : *rates) {
                if (const json* v = ju::find(rt, "rewards_daily_rate")) daily += ju::to_double(*v);
            }
        }
    }
    if (daily <= 0.0) return std::nullopt;

    std::string token;
    if (const json* toks = ju::find(market, "tokens")) {
        if (toks->is_array()) {
            for (const auto& tok : *toks) {
                const json* tid = ju::find(tok, "token_id");
                if (tid != nullptr && ju::truthy(*tid)) {
                    token = ju::to_str(*tid);
                    break;
                }
            }
        }
    }
    if (token.empty()) return std::nullopt;

    // max_spread/min_size/tick 任一为非数值 → 丢整个池 (对齐 Python try/except: return None)。
    const auto ms = py_float_or(rw, "max_spread", 0.0);
    const auto msz = py_float_or(rw, "min_size", 0.0);
    const auto tk = py_float_or(market, "minimum_tick_size", 0.01);
    if (!ms || !msz || !tk) return std::nullopt;

    RewardConfig c;
    c.daily = daily;
    c.max_spread = *ms;
    c.min_size = *msz;
    c.tick = *tk;
    c.token = token;
    std::string q;
    if (const json* v = ju::find(market, "question")) q = ju::to_str(*v);
    c.question = pmm::strutil::utf8_prefix(q, 80);  // 按码点截断 (不切坏 UTF-8 → 不会让 json.dump 抛)
    if (const json* v = ju::find(market, "condition_id")) c.condition_id = ju::to_str(*v);
    // 结算时间 end_date_iso ("YYYY-MM-DDTHH:MM:SSZ") → unix, 给近结算过滤用。
    if (const json* v = ju::find(market, "end_date_iso")) {
        const std::string ed = ju::to_str(*v);
        std::tm tm{};
        if (std::sscanf(ed.c_str(), "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                        &tm.tm_hour, &tm.tm_min, &tm.tm_sec) >= 3) {
            tm.tm_year -= 1900;
            tm.tm_mon -= 1;
            const std::time_t t = timegm(&tm);
            if (t > 0) c.end_date_unix = static_cast<double>(t);
        }
    }
    return c;
}

std::pair<double, double> inband_score(const json& levels, double mid, double max_spread_cents,
                                       bool is_bid) {
    const double c = max_spread_cents;
    double score = 0.0;
    double notional = 0.0;
    if (levels.is_array()) {
        for (const auto& lvl : levels) {
            const json* pp = ju::find(lvl, "price");
            const json* sp = ju::find(lvl, "size");
            if (pp == nullptr || sp == nullptr) continue;
            const double price = ju::to_double(*pp);
            const double size = ju::to_double(*sp);
            const double s_cents = ((is_bid ? (mid - price) : (price - mid))) * 100.0;
            if (s_cents >= -1e-9 && s_cents <= c + 1e-9) {
                const double w = c > 0.0 ? ((c - s_cents) / c) * ((c - s_cents) / c) : 0.0;
                score += size * w;
                notional += size * (is_bid ? price : (1.0 - price));
            }
        }
    }
    return {score, notional};
}

double reward_share(double min_size, double tick, double max_spread_cents,
                    double existing_min_side_score) {
    const double c = max_spread_cents;
    const double s_cents = tick * 100.0;
    const double my_w = c > 0.0 ? ((c - s_cents) / c) * ((c - s_cents) / c) : 0.0;
    const double my_score = min_size * my_w;
    const double denom = my_score + existing_min_side_score;
    return denom > 0.0 ? my_score / denom : 0.0;
}

JumpRisk classify_jump_risk(const std::vector<double>& prices, double reward_per_day, double min_size,
                            double kill_days, double watch_days) {
    JumpRisk jr;
    if (prices.size() < 10) {
        jr.verdict = "no-history";
        jr.days = static_cast<int>(prices.size());
        return jr;
    }
    std::vector<double> moves;
    moves.reserve(prices.size() - 1);
    for (std::size_t i = 1; i < prices.size(); ++i) moves.push_back(std::abs(prices[i] - prices[i - 1]));
    double max_jump = moves[0];
    double sum = 0.0;
    for (double m : moves) {
        max_jump = std::max(max_jump, m);
        sum += m;
    }
    const double mean = sum / static_cast<double>(moves.size());
    double var_sum = 0.0;
    for (double m : moves) var_sum += (m - mean) * (m - mean);
    const double full_vol = std::sqrt(var_sum / static_cast<double>(moves.size()));
    // 近窗波动 (最近 ~7 天): 抓"全史平静但近期变活跃"的池 (calm-before-catalyst, 如真人秀临近决赛)。
    // 报告 max(全史, 近窗) → 波动率过滤对"正在升温"的池也生效, 不只看长期平均。
    const std::size_t rw = std::min<std::size_t>(7, moves.size());
    double rsum = 0.0;
    for (std::size_t i = moves.size() - rw; i < moves.size(); ++i) rsum += moves[i];
    const double rmean = rsum / static_cast<double>(rw);
    double rvar = 0.0;
    for (std::size_t i = moves.size() - rw; i < moves.size(); ++i) rvar += (moves[i] - rmean) * (moves[i] - rmean);
    const double recent_vol = std::sqrt(rvar / static_cast<double>(rw));
    const double vol = full_vol;  // daily_vol_c 保持全史 (与 Python parity); 近窗单列 recent_vol_c 给过滤用
    const double jump_loss = min_size * max_jump;
    const double inf = std::numeric_limits<double>::infinity();
    const double days_wiped = reward_per_day > 0.0 ? (jump_loss / reward_per_day) : inf;

    if (days_wiped > kill_days) {
        jr.verdict = "KILL";
    } else if (days_wiped > watch_days) {
        jr.verdict = "WATCH";
    } else {
        jr.verdict = "SAFE";
    }
    jr.days = static_cast<int>(prices.size());
    jr.max_jump_c = roundn(max_jump * 100.0, 2);
    jr.daily_vol_c = roundn(vol * 100.0, 2);
    jr.recent_vol_c = roundn(recent_vol * 100.0, 2);
    jr.days_wiped = std::isinf(days_wiped) ? std::optional<double>{} : std::optional<double>{roundn(days_wiped, 1)};
    return jr;
}

std::optional<PoolReport> score_pool(const RewardConfig& pool, const json& book,
                                     const std::vector<orderbook::PricePoint>& history,
                                     double reward_calib) {
    const json* bids_p = ju::find(book, "bids");
    const json* asks_p = ju::find(book, "asks");
    const json bids = (bids_p != nullptr) ? *bids_p : json::array();
    const json asks = (asks_p != nullptr) ? *asks_p : json::array();

    const auto best_bid = best_price(bids, true);
    const auto best_ask = best_price(asks, false);
    if (!best_bid || !best_ask) return std::nullopt;

    const double mid = (*best_bid + *best_ask) / 2.0;
    // P2(选池): 极端价 (mid<0.10 / >0.90) — pin 风险 + 逆选择最重, 且 binding_qmin 强制双边、
    // 资本效率最差。小账户直接剔除这类池。
    if (mid < 0.10 || mid > 0.90) return std::nullopt;
    const double c = pool.max_spread;
    const auto [bscore, bnot] = inband_score(bids, mid, c, true);
    const auto [ascore, anot] = inband_score(asks, mid, c, false);
    const double min_side = pmm::orderbook::binding_qmin(bscore, ascore, mid);  // 官方绑定 Qmin
    // 利润校准 κ: 真实竞争 ≈ 快照 / κ (实测高估份额 4.2×; 你一挂上就被 farmer 稀释)。充气 → 份额降到真实
    // 水平。这是 SHARE_CEIL=0.40 拍脑袋封顶的"用数学说话"版 (κ 从真实结算反解)。
    const double min_side_calib = min_side / std::max(reward_calib, 1e-6);
    const double raw_share = reward_share(pool.min_size, pool.tick, c, min_side_calib);
    // P0(选池): 份额封顶。快照份额对薄盘口系统性高估 (薄=会吸引farmer→你一挂上份额就被稀释),
    // 高估恰好最大在它排第一的池。顶到 ~0.40, 与部署份额上限 0.30 一致, 砍掉幻想分。
    constexpr double SHARE_CEIL = 0.40;
    const double share = raw_share < SHARE_CEIL ? raw_share : SHARE_CEIL;
    const double capital = pool.min_size;
    const double reward_per_day = share * pool.daily;
    const double gross_ann = capital > 0.0 ? (reward_per_day * 365.0 / capital * 100.0) : 0.0;

    std::vector<double> prices;
    prices.reserve(history.size());
    for (const auto& pt : history) prices.push_back(pt.p);
    const JumpRisk jump = classify_jump_risk(prices, reward_per_day, pool.min_size);

    PoolReport r;
    r.question = pool.question;
    r.condition_id = pool.condition_id;
    r.token = pool.token;
    r.daily = roundn(pool.daily, 2);
    r.max_spread_c = c;
    r.min_size = pool.min_size;
    r.tick = pool.tick;
    r.mid = roundn(mid, 4);
    r.spread_c = roundn((*best_ask - *best_bid) * 100.0, 2);
    r.inband_notional = round_int(bnot + anot);
    r.share = roundn(share, 4);
    r.min_side_score = roundn(min_side, 4);
    r.empty_band = raw_share >= EMPTY_BAND_SHARE;  // 空带检测用未封顶的原始份额
    r.reward_per_day = roundn(reward_per_day, 2);
    r.gross_ann_pct = round_int(gross_ann);
    r.jump_verdict = jump.verdict;
    r.max_jump_c = jump.max_jump_c;
    r.daily_vol_c = jump.daily_vol_c;
    r.recent_vol_c = jump.recent_vol_c;
    r.days_wiped = jump.days_wiped;
    return r;
}

// ---------------------------------------------------------------------------
// RewardsClient
// ---------------------------------------------------------------------------

RewardsClient::RewardsClient(RateLimiter* rate_limiter, std::size_t pool_size)
    : rate_limiter_(rate_limiter), clob_(kClobHost, pool_size) {}

json RewardsClient::get(const std::string& path) {
    if (rate_limiter_ != nullptr) {
        rate_limiter_->acquire(path, "GET");  // 按端点分类 (/book 150/s, /sampling 50/s, ...)
    }
    const net::HttpResponse r = clob_.Get(path);
    if (r.status == 0) throw ApiError("Polymarket CLOB API request failed");
    if (r.status >= 400) {
        throw ApiError(std::format("Polymarket CLOB API error: {} {}", r.status, r.body.substr(0, 200)),
                       r.status);
    }
    try {
        return json::parse(r.body);
    } catch (...) {
        throw ApiError("Polymarket CLOB API: invalid JSON response");
    }
}

std::map<std::string, std::pair<double, double>> RewardsClient::reward_markets_multi(int max_pages) {
    std::map<std::string, std::pair<double, double>> out;
    std::string cursor;
    for (int i = 0; i < max_pages; ++i) {
        std::string path = "/rewards/markets/multi?page_size=500";
        if (!cursor.empty()) path += "&next_cursor=" + cursor;
        json data;
        try {
            data = get(path);
        } catch (...) {
            break;  // 失败不致命: 选池退回不带这层增强
        }
        const json* d = data.is_object() ? ju::find(data, "data") : nullptr;
        const json page = (d != nullptr) ? *d : (data.is_array() ? data : json::array());
        if (!page.is_array() || page.empty()) break;
        for (const auto& m : page) {
            const json* cv = ju::find(m, "condition_id");
            if (cv == nullptr) continue;
            const std::string cond = ju::to_str(*cv);
            if (cond.empty()) continue;
            double compet = 0.0;
            if (const json* c = ju::find(m, "market_competitiveness")) compet = ju::to_double(*c);
            double remaining = -1.0;
            if (const json* rc = ju::find(m, "rewards_config")) {
                if (rc->is_array() && !rc->empty()) {
                    remaining = 0.0;
                    for (const auto& cfg : *rc)
                        if (const json* rem = ju::find(cfg, "remaining_reward_amount"))
                            remaining += ju::to_double(*rem);
                }
            }
            out[cond] = {compet, remaining};
        }
        std::string nxt;
        if (data.is_object())
            if (const json* n = ju::find(data, "next_cursor")) nxt = ju::to_str(*n);
        if (nxt.empty() || nxt == "LTE=" || nxt == cursor) break;
        cursor = nxt;
    }
    return out;
}

std::vector<json> RewardsClient::sampling_markets(int max_pages) {
    std::vector<json> out;
    std::string cursor;
    for (int i = 0; i < max_pages; ++i) {
        std::string path = "/sampling-markets";
        if (!cursor.empty()) {
            // next_cursor 是 base64url, 含 '='; 这里直接拼 (PM 接受)。
            path += "?next_cursor=" + cursor;
        }
        const json data = get(path);
        json page;
        if (data.is_object()) {
            const json* d = ju::find(data, "data");
            page = (d != nullptr) ? *d : json::array();
        } else {
            page = data;
        }
        if (!page.is_array() || page.empty()) break;
        for (const auto& m : page) out.push_back(m);
        std::string nxt;
        if (data.is_object()) {
            if (const json* n = ju::find(data, "next_cursor")) nxt = ju::to_str(*n);
        }
        if (nxt.empty() || nxt == "LTE=" || nxt == cursor) break;
        cursor = nxt;
    }
    return out;
}

json RewardsClient::book(const std::string& token_id) {
    const json data = get("/book?token_id=" + token_id);
    return data.is_object() ? data : json::object();
}

std::vector<orderbook::PricePoint> RewardsClient::prices_history(const std::string& token_id,
                                                                const std::string& interval,
                                                                int fidelity) {
    const json data =
        get("/prices-history?market=" + token_id + "&interval=" + interval +
            "&fidelity=" + std::to_string(fidelity));
    std::vector<orderbook::PricePoint> out;
    if (data.is_object()) {
        if (const json* h = ju::find(data, "history")) {
            if (h->is_array()) {
                for (const auto& pt : *h) {
                    const json* p = ju::find(pt, "p");
                    const json* t = ju::find(pt, "t");
                    if (p == nullptr || t == nullptr) continue;
                    out.push_back({ju::to_double(*p), ju::to_double(*t)});
                }
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// scan
// ---------------------------------------------------------------------------

ScanResult scan(RewardsClient& client, double min_daily, int top, bool with_jump_risk,
                double min_days_to_resolution, double max_vol_mult, double reward_calib,
                const std::set<std::string>& whitelist) {
    const std::vector<json> markets = client.sampling_markets();
    const double now = static_cast<double>(std::time(nullptr));

    int total_reward_pools = 0;
    std::vector<RewardConfig> pools;
    for (const auto& m : markets) {
        auto p = parse_rewards(m);
        if (p) {
            ++total_reward_pools;
            if (p->daily < min_daily) continue;
            // 近结算过滤 (R2 金融家): 临近结算 = 灾难性跳变/pin 风险, 被动做市必被碾, 且奖励流短。
            if (min_days_to_resolution > 0.0 && p->end_date_unix > 0.0 &&
                (p->end_date_unix - now) / 86400.0 < min_days_to_resolution)
                continue;
            // 催化剂关键词过滤 (R3): 代币上线/IPO/空投 — end_date 远但催化剂近, 日期过滤抓不到,
            // 上线日 FDV 会暴力跳变碾过被动做市单 (实测 round-2 fuse-fdv 漏网)。
            if (min_days_to_resolution > 0.0) {
                std::string ql = p->question;
                for (char& ch : ql) ch = static_cast<char>((ch >= 'A' && ch <= 'Z') ? ch + 32 : ch);
                // R3+: 代币/产品上线 + 影视/赛事首映 + 估值里程碑 = 近期暴力跳变 (实测漏网: gpt-released /
                // spider-man-opening / stripe-valuation)。end_date 远但催化剂近, 日期过滤抓不到。
                static const char* kCatalyst[] = {"fdv",       "launch",  " ipo",      "debut",
                                                  "airdrop",   "listing", "release",   "premiere",
                                                  "valuation", "opening weekend"};
                bool catalyst = false;
                for (const char* kw : kCatalyst)
                    if (ql.find(kw) != std::string::npos) {
                        catalyst = true;
                        break;
                    }
                if (catalyst) continue;
            }
            pools.push_back(*p);
        }
    }
    std::sort(pools.begin(), pools.end(),
              [](const RewardConfig& a, const RewardConfig& b) { return a.daily > b.daily; });
    if (top > 0 && static_cast<int>(pools.size()) > top) {
        pools.resize(static_cast<std::size_t>(top));
    }

    auto score_one = [&](const RewardConfig& p) -> std::optional<PoolReport> {
        json bk;
        try {
            bk = client.book(p.token);
        } catch (const ApiError&) {
            return std::nullopt;
        }
        std::vector<orderbook::PricePoint> history;
        if (with_jump_risk) {
            try {
                history = client.prices_history(p.token);
            } catch (const ApiError&) {
                history.clear();
            }
        }
        return score_pool(p, bk, history, reward_calib);
    };

    std::vector<std::optional<PoolReport>> results(pools.size());
    if (!pools.empty()) {
        const int workers = std::min<int>(SCAN_WORKERS, static_cast<int>(pools.size()));
        std::atomic<std::size_t> next{0};
        auto worker = [&] {
            for (;;) {
                const std::size_t i = next.fetch_add(1);
                if (i >= pools.size()) break;
                results[i] = score_one(pools[i]);
            }
        };
        std::vector<std::thread> ths;
        ths.reserve(static_cast<std::size_t>(workers));
        for (int w = 0; w < workers; ++w) ths.emplace_back(worker);
        for (auto& t : ths) t.join();
    }

    // B: PM 官方权威 竞争度 + 池剩余额度 (失败则退回不带这层增强)。
    std::map<std::string, std::pair<double, double>> multi;
    try {
        multi = client.reward_markets_multi();
    } catch (...) {
    }
    std::vector<PoolReport> scored;
    int vol_dropped = 0;
    int depleted_dropped = 0;
    for (auto& r : results) {
        if (!r) continue;
        // 波动率过滤: 实现日波动 > vol_mult×奖励带宽 的池太跳, 逆选择重 → 剔除。
        // 历史无数据 (daily_vol_c 空) 的不剔。注意: 只抓"历史就跳"的, 抓不到"决赛前平静"的未来催化剂
        // (那类靠 max_mid_vel_cps 入场护栏 + FAK 自平 + 安全退出兜底)。
        // 取 全史 与 近窗 波动的较大者 → 既抓"长期就跳"也抓"近期升温"(calm-before-catalyst, 如真人秀临近决赛)。
        const double vol_c = std::max(r->daily_vol_c.value_or(0.0), r->recent_vol_c.value_or(0.0));
        // 白名单池绕过"跳动剔除": LLM 选的宽基行为池常因临近赛事/比赛跳动被扫描层剔掉, 但那正是有散户
        // 行为盈余 + fade 抗跳的地方 → 放行进入可做集 (功能性的 depleted<$1 仍剔)。
        if (max_vol_mult > 0.0 && whitelist.count(r->condition_id) == 0 && vol_c > 0.0 &&
            r->max_spread_c > 0.0 && vol_c > max_vol_mult * r->max_spread_c) {
            ++vol_dropped;
            continue;
        }
        // B: 接 PM 权威数据。剔除快发完的池 (剩余已知且 <$1 = 没价值); 竞争度附上供选池/感知。
        auto mit = multi.find(r->condition_id);
        if (mit != multi.end()) {
            r->competitiveness = mit->second.first;
            r->remaining_reward = mit->second.second;
            if (r->remaining_reward >= 0.0 && r->remaining_reward < 1.0) {
                ++depleted_dropped;
                continue;
            }
        }
        scored.push_back(std::move(*r));
    }
    if (vol_dropped > 0 || depleted_dropped > 0)
        std::fprintf(stderr, "scan: dropped %d jumpy (vol>%.1fx band) + %d depleted (remaining<$1) pool(s)\n",
                     vol_dropped, max_vol_mult, depleted_dropped);

    // 排序: SAFE 先, 再 非空簿 高毛年化。
    auto verdict_rank = [](const std::string& v) -> int {
        if (v == "SAFE") return 0;
        if (v == "WATCH") return 1;
        if (v == "no-history") return 2;
        if (v == "KILL") return 3;
        return 9;  // 含 "" / 其它
    };
    std::sort(scored.begin(), scored.end(), [&](const PoolReport& a, const PoolReport& b) {
        if (a.empty_band != b.empty_band) return !a.empty_band;  // False(0) 先
        const int ra = verdict_rank(a.jump_verdict);
        const int rb = verdict_rank(b.jump_verdict);
        if (ra != rb) return ra < rb;
        if (a.gross_ann_pct != b.gross_ann_pct) return a.gross_ann_pct > b.gross_ann_pct;  // 毛年化降序
        // B: 毛收益相近时, 优先 PM 竞争度更低 (更不拥挤) 的池; 竞争度未知(-1)排后。
        const double ca = a.competitiveness < 0.0 ? 1e18 : a.competitiveness;
        const double cb = b.competitiveness < 0.0 ? 1e18 : b.competitiveness;
        return ca < cb;
    });

    int safe_count = 0;
    for (const auto& r : scored) {
        if (r.jump_verdict == "SAFE" && !r.empty_band) ++safe_count;
    }

    ScanResult res;
    res.min_daily = min_daily;
    res.top = top;
    res.with_jump_risk = with_jump_risk;
    res.total_reward_pools = total_reward_pools;
    res.pools_scored = static_cast<int>(scored.size());
    res.safe_count = safe_count;
    res.pools = std::move(scored);
    return res;
}

}  // namespace pmm::rewards
