// src/pmm/engine.cpp — 交易执行引擎 (port of pm_trader/engine.py, 做市编排路径)
#include "pmm/engine.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "pmm/orderbook.hpp"
#include "pmm/round.hpp"

namespace pmm {

namespace ob = pmm::orderbook;
using nlohmann::json;

namespace {

double now_unix_default() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string unix_to_iso(double unix) {
    std::time_t t = static_cast<std::time_t>(std::floor(unix));
    long us = std::lround((unix - std::floor(unix)) * 1e6);
    if (us >= 1000000) {
        us -= 1000000;
        t += 1;
    }
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[48];
    if (us == 0) {  // Python isoformat() 在 microsecond==0 时省略小数部分
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d+00:00", tm.tm_year + 1900,
                      tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%06ld+00:00", tm.tm_year + 1900,
                      tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, us);
    }
    return buf;
}

double iso_to_unix(const std::string& s) {
    int Y = 0, M = 0, D = 0, h = 0, mi = 0, sec = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &mi, &sec) != 6) return 0.0;
    double frac = 0.0;
    const auto dot = s.find('.');
    if (dot != std::string::npos) frac = std::atof(s.c_str() + dot);  // ".ffffff[+tz]" → 0.ffffff
    std::tm tm{};
    tm.tm_year = Y - 1900;
    tm.tm_mon = M - 1;
    tm.tm_mday = D;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = sec;
    return static_cast<double>(timegm(&tm)) + frac;  // 时间戳均 UTC (+00:00)
}

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c);
    return s;
}
std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

}  // namespace

Engine::Engine(std::filesystem::path data_dir) : db_(std::move(data_dir)), api_(db_) {
    db_.init_schema();
    db_.init_orders_schema();
}

void Engine::close() {
    db_.close();
}

// ---- account ----

Account Engine::init_account(double balance) {
    Account a = db_.init_account(balance);
    record_equity();  // seed 起始(flat)权益
    return a;
}

Account Engine::get_account() {
    auto a = db_.get_account();
    if (!a) throw NotInitializedError();
    return *a;
}

Account Engine::require_account() {
    return get_account();
}

void Engine::reset() {
    db_.reset();
    db_.init_orders_schema();
}

std::string Engine::validate_outcome(const std::string& outcome_in, const Market* market) {
    const std::string outcome = trim(to_lower(outcome_in));
    if (outcome.empty()) throw InvalidOutcomeError(outcome);
    if (market != nullptr) {
        bool found = false;
        for (const auto& o : market->outcomes) {
            if (to_lower(o) == outcome) {
                found = true;
                break;
            }
        }
        if (!found) throw InvalidOutcomeError(outcome);
    }
    return outcome;
}

std::optional<double> Engine::book_mid(const OrderBook& book) {
    if (book.bids.empty() || book.asks.empty()) return std::nullopt;
    double best_bid = book.bids.front().price;
    for (const auto& l : book.bids) best_bid = std::max(best_bid, l.price);
    double best_ask = book.asks.front().price;
    for (const auto& l : book.asks) best_ask = std::min(best_ask, l.price);
    return (best_bid + best_ask) / 2.0;
}

double Engine::committed_maker_capital() {
    double sum = 0.0;
    for (const auto& q : db_.get_active_maker_quotes()) sum += q.committed_capital;
    return sum;
}

void Engine::record_equity(const std::optional<Mark>& mark) {
    try {
        auto account = db_.get_account();
        if (!account) return;
        double equity = account->cash + committed_maker_capital();
        for (const auto& pos : db_.get_open_positions()) {
            double price;
            if (mark.has_value() && pos.market_condition_id == mark->condition_id &&
                pos.outcome == mark->outcome) {
                price = mark->price;
            } else {
                price = pos.avg_entry_price;
            }
            equity += pos.shares * price;
        }
        db_.record_equity(equity);
    } catch (...) {
        // 记账绝不打断交易
    }
}

double Engine::snapshot_equity() {
    Account account = require_account();
    double equity = account.cash + committed_maker_capital();
    for (const auto& pos : db_.get_open_positions()) {
        double price = 0.0;
        try {
            Market m = api_.get_market(pos.market_slug);
            price = api_.get_midpoint(m.get_token_id(pos.outcome));
        } catch (...) {
            price = 0.0;
        }
        if (price <= 0.0) price = pos.avg_entry_price;
        equity += pos.shares * price;
    }
    db_.record_equity(equity);
    return equity;
}

// ---- 并发预取 (config + book + mid) ----

std::map<int, Engine::QuoteRead> Engine::prefetch_quote_reads(const std::vector<MakerQuote>& quotes) {
    std::map<int, QuoteRead> out;
    if (quotes.empty()) return out;
    std::vector<QuoteRead> reads(quotes.size());

    auto read_one = [&](const MakerQuote& q) -> QuoteRead {
        QuoteRead r;
        try {
            r.pool = api_.get_reward_config(q.market_condition_id);
            r.config_ok = true;
        } catch (...) {
            return r;  // transient → 下轮重试
        }
        if (!r.pool || r.pool->daily <= 0.0) return r;  // reconcile/exit 不需要 book
        // 优先实时 WS book (连接活 + 两边有效); 否则 REST (退化优雅)。
        if (book_source_ != nullptr && book_source_->is_live()) {
            auto b = book_source_->get_book(q.token_id);
            const double m = book_source_->get_midpoint(q.token_id);
            if (b && 0.0 < m && m < 1.0) {
                r.book = std::move(*b);
                r.mid = m;
                r.book_ok = true;
                return r;
            }
        }
        try {
            r.book = api_.get_order_book(q.token_id);
            r.mid = api_.get_midpoint(q.token_id);
            r.book_ok = true;
        } catch (...) {
        }
        return r;
    };

    const int workers = std::min<int>(MAKER_PREFETCH_WORKERS, static_cast<int>(quotes.size()));
    std::atomic<std::size_t> next{0};
    auto worker = [&] {
        for (;;) {
            const std::size_t i = next.fetch_add(1);
            if (i >= quotes.size()) break;
            reads[i] = read_one(quotes[i]);
        }
    };
    std::vector<std::thread> ths;
    ths.reserve(static_cast<std::size_t>(workers));
    for (int w = 0; w < workers; ++w) ths.emplace_back(worker);
    for (auto& t : ths) t.join();

    for (std::size_t i = 0; i < quotes.size(); ++i) out[quotes[i].id] = std::move(reads[i]);
    return out;
}

void Engine::credit_maker(double amount) {
    db_.update_cash(get_account().cash + amount);
}

bool Engine::action_ok(const json& res) {
    if (res.is_object()) {
        const auto it = res.find("status");
        if (it != res.end() && it->is_string()) {
            const std::string st = it->get<std::string>();
            return st != "ERROR" && st != "REJECTED";
        }
    }
    return true;
}

json Engine::flatten_live(const maker::Submitter& submitter, const std::string& token_id,
                          double inventory) {
    if (std::abs(inventory) < 1e-9) return nullptr;
    const std::string side = inventory > 0 ? "SELL" : "BUY";
    return submitter({{"action", "FLATTEN"}, {"token_id", token_id}, {"side", side},
                      {"size", std::abs(inventory)}});
}

void Engine::exit_maker_quote(const MakerQuote& quote, double mid, const std::string& reason,
                              std::vector<json>& results, double crossing_cost, const json& extra) {
    db_.update_cash(get_account().cash + quote.committed_capital - std::max(0.0, crossing_cost));
    auto cancelled = db_.cancel_maker_quote(quote.id);
    json row;
    row["quote"] = cancelled ? maker_quote_to_dict(*cancelled) : maker_quote_to_dict(quote);
    row["reconciled"] = reason;
    row["mid"] = mid;
    if (crossing_cost != 0.0) row["crossing_cost"] = round_to(crossing_cost, 6);
    if (extra.is_object()) {
        for (auto it = extra.begin(); it != extra.end(); ++it) row[it.key()] = round_to(it.value().get<double>(), 6);
    }
    results.push_back(std::move(row));
}

// ---- place_maker_quote (paper) ----

json Engine::place_maker_quote(const std::string& slug_or_id, const std::string& outcome_in,
                               const MakerQuoteOpts& opts) {
    Account account = require_account();
    Market market = api_.get_market(slug_or_id);
    const std::string outcome = validate_outcome(outcome_in, &market);
    if (market.closed) throw MarketClosedError(market.slug);

    auto pool = api_.get_reward_config(market.condition_id);
    if (!pool) throw OrderRejectedError(market.slug + " is not in the liquidity-rewards program");
    const double max_spread_c = pool->max_spread;
    const double min_size = pool->min_size;
    const double daily_rate = pool->daily;
    const double tick = pool->tick;

    const double size = opts.size.value_or(min_size);
    if (size < min_size) throw OrderRejectedError("Maker size below pool min_size");
    const double half_spread_c = opts.half_spread_cents.value_or(tick * 100.0);
    if (half_spread_c <= 0.0 || half_spread_c > max_spread_c)
        throw OrderRejectedError("half_spread_cents out of (0, max_spread]");
    if (!(opts.cancel_efficiency >= 0.0 && opts.cancel_efficiency <= 1.0))
        throw OrderRejectedError("cancel_efficiency must be in [0, 1]");
    if (opts.skew_strength < 0.0) throw OrderRejectedError("skew_strength must be >= 0");
    const double cap_shares = opts.max_inventory.value_or(MAKER_CAP_MULT * size);
    if (cap_shares <= 0.0) throw OrderRejectedError("max_inventory must be > 0");

    const std::string token_id = market.get_token_id(outcome);
    // 互补 (NO) token: 二元市场里另一个 token — ask 腿改挂 BUY-NO 才能纯 USDC 双边。
    std::string complement_token_id;
    for (const auto& t : market.tokens) {
        if (t.token_id != token_id && !t.token_id.empty()) {
            complement_token_id = t.token_id;
            break;
        }
    }
    const double mid = api_.get_midpoint(token_id);
    if (!(0.0 < mid && mid < 1.0)) throw OrderRejectedError("No valid midpoint to anchor the maker quote");

    const double offset = half_spread_c / 100.0;
    const double min_leg = size * std::min(mid - offset, 1.0 - mid - offset);
    if (min_leg < MIN_ORDER_USD) throw OrderRejectedError("Maker leg notional below exchange minimum");

    const double cap = ob::committed_capital(size, half_spread_c);
    if (cap > account.cash) throw InsufficientBalanceError(cap, account.cash);
    db_.update_cash(account.cash - cap);

    MakerQuoteInput in;
    in.market_slug = market.slug;
    in.market_condition_id = market.condition_id;
    in.outcome = outcome;
    in.token_id = token_id;
    in.complement_token_id = complement_token_id;
    in.size = size;
    in.half_spread_c = half_spread_c;
    in.max_spread_c = max_spread_c;
    in.min_size = min_size;
    in.daily_rate = daily_rate;
    in.tick = tick;
    in.cancel_efficiency = opts.cancel_efficiency;
    in.max_inventory = cap_shares;
    in.skew_strength = opts.skew_strength;
    in.entry_mid = mid;
    in.committed_capital = cap;
    in.last_mid = mid;
    in.last_accrued_at = unix_to_iso(opts.now_unix.value_or(now_unix_default()));
    const MakerQuote quote = db_.create_maker_quote(in);
    record_equity();
    return maker_quote_to_dict(quote);
}

// ---- accrue_maker_rewards (paper) ----

std::vector<json> Engine::accrue_maker_rewards(std::optional<double> now_unix) {
    require_account();
    const double now = now_unix.value_or(now_unix_default());
    std::vector<json> results;
    const std::vector<MakerQuote> quotes = db_.get_active_maker_quotes();
    const auto reads = prefetch_quote_reads(quotes);

    for (const auto& quote : quotes) {
        auto it = reads.find(quote.id);
        const QuoteRead r = (it != reads.end()) ? it->second : QuoteRead{};
        if (!r.config_ok) continue;
        const double daily_rate = r.pool ? r.pool->daily : 0.0;
        if (!r.pool || daily_rate <= 0.0) {
            exit_maker_quote(quote, quote.last_mid, "rewards_ended", results,
                             maker_crossing_cost_c_ / 100.0 * std::abs(quote.inventory));
            continue;
        }
        if (!r.book_ok) continue;
        const OrderBook& book = r.book;
        const double mid = r.mid;
        if (!(0.0 < mid && mid < 1.0)) continue;

        const double last = iso_to_unix(quote.last_accrued_at);
        const double seconds = std::max(0.0, now - last);

        const double existing_qmin = ob::book_inband_qmin(book, mid, quote.max_spread_c);
        const double share = ob::maker_reward_share(quote.size, quote.half_spread_c, quote.max_spread_c,
                                                    existing_qmin);
        const double reward = ob::reward_accrual(share, daily_rate, seconds);

        const double cap = quote.max_inventory > 0.0 ? quote.max_inventory : MAKER_CAP_MULT * quote.size;
        const double held_mtm = quote.inventory * (mid - quote.last_mid);
        const auto [d_inv, fill_loss] = ob::maker_fill(quote.last_mid, mid, quote.inventory, quote.size,
                                                       quote.half_spread_c, quote.skew_strength,
                                                       quote.cancel_efficiency, cap);
        const double new_inventory = quote.inventory + d_inv;
        const double inv_pnl_delta = held_mtm - fill_loss;

        const double entry_mid = quote.entry_mid > 0.0 ? quote.entry_mid : mid;
        if (std::abs(mid - entry_mid) >= quote.max_spread_c / 100.0) {
            credit_maker(reward + inv_pnl_delta);
            AccrualUpdate u;
            u.accrued_rewards = quote.accrued_rewards + reward;
            u.realized_bleed = quote.realized_bleed + fill_loss;
            u.fills = quote.fills + (d_inv != 0.0 ? 1 : 0);
            u.last_mid = mid;
            u.last_accrued_at = unix_to_iso(now);
            u.inventory = 0.0;
            u.inventory_pnl = quote.inventory_pnl + inv_pnl_delta;
            const MakerQuote final_q = db_.update_maker_quote_accrual(quote.id, u);
            json extra = {{"reward", reward}, {"fill_loss", fill_loss},
                          {"inventory_pnl_delta", inv_pnl_delta}, {"share", share}, {"seconds", seconds}};
            exit_maker_quote(final_q, mid, "drift_exit", results,
                             maker_crossing_cost_c_ / 100.0 * std::abs(new_inventory), extra);
            continue;
        }

        credit_maker(reward + inv_pnl_delta);
        AccrualUpdate u;
        u.accrued_rewards = quote.accrued_rewards + reward;
        u.realized_bleed = quote.realized_bleed + fill_loss;
        u.fills = quote.fills + (d_inv != 0.0 ? 1 : 0);
        u.last_mid = mid;
        u.last_accrued_at = unix_to_iso(now);
        u.inventory = new_inventory;
        u.inventory_pnl = quote.inventory_pnl + inv_pnl_delta;
        const MakerQuote updated = db_.update_maker_quote_accrual(quote.id, u);
        results.push_back({{"quote", maker_quote_to_dict(updated)},
                           {"reward", round_to(reward, 6)},
                           {"fill_loss", round_to(fill_loss, 6)},
                           {"inventory_pnl_delta", round_to(inv_pnl_delta, 6)},
                           {"inventory", round_to(new_inventory, 4)},
                           {"share", round_to(share, 6)},
                           {"seconds", round_to(seconds, 2)},
                           {"mid", mid}});
    }
    record_equity();
    return results;
}

// ---- place_maker_quote_live ----

json Engine::place_maker_quote_live(const std::string& slug_or_id, const maker::Submitter& submitter,
                                    const std::string& outcome, const MakerQuoteOpts& opts) {
    // Python place_maker_quote_live 不传 cancel_efficiency → paper 侧用默认 0 (live 用真实成交, 该字段不影响 P&L)。
    MakerQuoteOpts popts = opts;
    popts.cancel_efficiency = 0.0;
    json q = place_maker_quote(slug_or_id, outcome, popts);
    const double mid = q["entry_mid"].get<double>() != 0.0 ? q["entry_mid"].get<double>()
                                                           : q["last_mid"].get<double>();
    const std::string yes_token = q.value("token_id", std::string{});
    const std::string no_token = q.value("complement_token_id", std::string{});
    // 双边 = BUY-YES @ bid (yes_token) + BUY-NO @ (1-ask) (no_token)。纯 USDC, 不挂 SELL。
    const std::vector<maker::Order> orders = maker::compute_two_sided_quotes_yes_no(
        mid, q["half_spread_c"].get<double>(), q["size"].get<double>(), q["tick"].get<double>(),
        q["max_spread_c"].get<double>(), yes_token, no_token, 0.0);
    json acks = json::array();
    bool ok = true;
    for (const auto& o : orders) {
        if (o.token_id.empty()) continue;  // 互补 token 缺失 (非二元市场) → 跳过该腿, 不发空 token 单
        json ack;
        try {
            ack = submitter({{"action", "PLACE"}, {"token_id", o.token_id}, {"side", o.side},
                             {"price", o.price}, {"size", o.size}});
        } catch (...) {
            ack = {{"status", "ERROR"}};
        }
        acks.push_back(ack);
        if (!action_ok(ack)) ok = false;
    }
    q["submitted"] = acks;
    if (!ok) {
        // 把每条腿真实的 CLOB 应答 (http + errorMsg) 打出来 — 否则只知道"被拒"不知为何。
        std::fprintf(stderr, "place_maker_quote_live REJECTED yes=%s no=%s mid=%.4f acks=%s\n",
                     yes_token.c_str(), no_token.c_str(), mid, acks.dump().c_str());
        submitter({{"action", "CANCEL_ALL"}, {"token_id", yes_token}});
        if (!no_token.empty()) submitter({{"action", "CANCEL_ALL"}, {"token_id", no_token}});
        cancel_maker_quote(q["id"].get<int>());
        throw OrderRejectedError("maker quote placement failed (rolled back)");
    }
    return q;
}

// ---- accrue_maker_rewards_live ----

std::vector<json> Engine::accrue_maker_rewards_live(const maker::Submitter& submitter,
                                                    const FillsByToken& fills_by_token,
                                                    std::optional<double> now_unix, int recenter_ticks,
                                                    const std::set<std::string>* force_recenter) {
    require_account();
    const double now = now_unix.value_or(now_unix_default());
    std::vector<json> results;
    const std::vector<MakerQuote> quotes = db_.get_active_maker_quotes();
    const auto reads = prefetch_quote_reads(quotes);

    for (const auto& quote : quotes) {
        auto it = reads.find(quote.id);
        const QuoteRead r = (it != reads.end()) ? it->second : QuoteRead{};
        if (!r.config_ok) continue;
        const double daily_rate = r.pool ? r.pool->daily : 0.0;
        const std::string& no_token = quote.complement_token_id;
        auto fit = fills_by_token.find(quote.token_id);
        auto nfit = no_token.empty() ? fills_by_token.end() : fills_by_token.find(no_token);
        const std::vector<RealFill> empty;
        const std::vector<RealFill>& fills = (fit != fills_by_token.end()) ? fit->second : empty;
        const std::vector<RealFill>& no_fills = (nfit != fills_by_token.end()) ? nfit->second : empty;

        // 撤掉两条腿 (BUY-YES on token_id + BUY-NO on complement_token_id)。
        auto cancel_both = [&]() -> bool {
            bool okc = action_ok(submitter({{"action", "CANCEL_ALL"}, {"token_id", quote.token_id}}));
            if (!no_token.empty())
                okc = action_ok(submitter({{"action", "CANCEL_ALL"}, {"token_id", no_token}})) && okc;
            return okc;
        };

        // 1. RECONCILE — 池退出/结算 → 撤两腿 + 卖掉两边持仓 + exit
        if (!r.pool || daily_rate <= 0.0) {
            double rd_pnl = 0.0, rd_bleed = 0.0, yes_inv = quote.inventory, no_inv = 0.0;
            int rn = 0;
            for (const auto& rf : fills) {  // YES 腿 (mid 空间)
                const double sgn = (to_lower(rf.side) == "buy") ? 1.0 : -1.0;
                const double fp = sgn * (quote.last_mid - rf.price) * rf.size;
                yes_inv += sgn * rf.size; rd_pnl += fp; rd_bleed += std::max(0.0, -fp); ++rn;
            }
            for (const auto& rf : no_fills) {  // NO 腿 (NO mid = 1 - yes_mid)
                const double sgn = (to_lower(rf.side) == "buy") ? 1.0 : -1.0;
                const double fp = sgn * ((1.0 - quote.last_mid) - rf.price) * rf.size;
                no_inv += sgn * rf.size; rd_pnl += fp; rd_bleed += std::max(0.0, -fp); ++rn;
            }
            const bool okc = cancel_both();
            const bool fy = action_ok(flatten_live(submitter, quote.token_id, yes_inv));
            const bool fn = no_token.empty() || action_ok(flatten_live(submitter, no_token, no_inv));
            credit_maker(rd_pnl);
            AccrualUpdate u;
            u.accrued_rewards = quote.accrued_rewards;
            u.realized_bleed = quote.realized_bleed + rd_bleed;
            u.fills = quote.fills + rn;
            u.last_mid = quote.last_mid;
            u.last_accrued_at = unix_to_iso(now);
            u.inventory_pnl = quote.inventory_pnl + rd_pnl;
            if (!(okc && fy && fn)) {
                if (!fn && std::abs(no_inv) >= 1e-9)
                    std::fprintf(stderr,
                                 "WARN orphaned NO inventory %.4f on %s (flatten failed) — flatten manually\n",
                                 no_inv, no_token.c_str());
                u.inventory = yes_inv;
                db_.update_maker_quote_accrual(quote.id, u);
                results.push_back({{"quote", maker_quote_to_dict(quote)},
                                   {"exit_failed", "rewards_ended"}, {"mid", quote.last_mid}});
                continue;
            }
            u.inventory = 0.0;
            const MakerQuote final_q = db_.update_maker_quote_accrual(quote.id, u);
            exit_maker_quote(final_q, quote.last_mid, "rewards_ended", results,
                             maker_crossing_cost_c_ / 100.0 * (std::abs(yes_inv) + std::abs(no_inv)),
                             json{{"inventory_pnl_delta", rd_pnl}});
            continue;
        }
        if (!r.book_ok) continue;
        const OrderBook& book = r.book;
        const double mid = r.mid;
        if (!(0.0 < mid && mid < 1.0)) continue;

        const double last = iso_to_unix(quote.last_accrued_at);
        const double seconds = std::min(std::max(0.0, now - last), MAX_ACCRUAL_SECONDS);

        const double existing_qmin = ob::book_inband_qmin(book, mid, quote.max_spread_c);
        const double share = ob::maker_reward_share(quote.size, quote.half_spread_c, quote.max_spread_c,
                                                    existing_qmin);
        const double reward = ob::reward_accrual(share, daily_rate, seconds);

        // 成交盈亏: YES 腿在 mid 空间; NO 腿在 (1-mid) 空间。两腿都是 BUY → 只做多。
        double d_yes = 0.0, d_no = 0.0, fill_pnl = 0.0, bleed = 0.0;
        int n_fills = 0;
        for (const auto& f : fills) {
            const double sgn = (to_lower(f.side) == "buy") ? 1.0 : -1.0;
            const double fp = sgn * (mid - f.price) * f.size;
            d_yes += sgn * f.size; fill_pnl += fp; bleed += std::max(0.0, -fp); ++n_fills;
        }
        for (const auto& f : no_fills) {
            const double sgn = (to_lower(f.side) == "buy") ? 1.0 : -1.0;
            const double fp = sgn * ((1.0 - mid) - f.price) * f.size;
            d_no += sgn * f.size; fill_pnl += fp; bleed += std::max(0.0, -fp); ++n_fills;
        }
        const double yes_inv = quote.inventory + d_yes;
        const double no_inv = d_no;
        const double pnl_delta = quote.inventory * (mid - quote.last_mid) + fill_pnl;

        // 2. 任一腿被吃 → 失败安全: 撤两腿 + 卖掉两边持仓 + 退出该池
        //    (v1 保守, 不在双币上做净额/skew; 被成交即退场, cooldown 后可重选)。
        if (n_fills > 0) {
            const bool okc = cancel_both();
            const bool fy = action_ok(flatten_live(submitter, quote.token_id, yes_inv));
            const bool fn = no_token.empty() || action_ok(flatten_live(submitter, no_token, no_inv));
            credit_maker(reward + pnl_delta);
            AccrualUpdate u;
            u.accrued_rewards = quote.accrued_rewards + reward;
            u.realized_bleed = quote.realized_bleed + bleed;
            u.fills = quote.fills + n_fills;
            u.last_mid = mid;
            u.last_accrued_at = unix_to_iso(now);
            u.inventory_pnl = quote.inventory_pnl + pnl_delta;
            if (!(okc && fy && fn)) {
                // NO 腿 flatten 失败时 no_inv 无列持久化 → 真实 NO 份额会被孤立。先大声告警 (人工/监督处理),
                // 完整修复 = 给 maker_quotes 加 complement_inventory 列并重试。YES 腿经 inventory 已会重试。
                if (!fn && std::abs(no_inv) >= 1e-9)
                    std::fprintf(stderr,
                                 "WARN orphaned NO inventory %.4f on %s (flatten failed) — flatten manually\n",
                                 no_inv, no_token.c_str());
                u.inventory = yes_inv;
                db_.update_maker_quote_accrual(quote.id, u);
                results.push_back({{"quote", maker_quote_to_dict(quote)}, {"exit_failed", "filled_exit"},
                                   {"mid", mid}, {"inventory", round_to(yes_inv, 4)}});
                continue;
            }
            u.inventory = 0.0;
            const MakerQuote final_q = db_.update_maker_quote_accrual(quote.id, u);
            exit_maker_quote(final_q, mid, "filled_exit", results,
                             maker_crossing_cost_c_ / 100.0 * (std::abs(yes_inv) + std::abs(no_inv)),
                             json{{"reward", reward}, {"inventory_pnl_delta", pnl_delta},
                                  {"share", share}, {"seconds", seconds}});
            continue;
        }

        // 3. DRIFT-EXIT — mid 漂出 max_spread → 撤两腿 + exit (无成交, 无持仓)。
        const double entry_mid = quote.entry_mid > 0.0 ? quote.entry_mid : mid;
        if (std::abs(mid - entry_mid) >= quote.max_spread_c / 100.0) {
            const bool okc = cancel_both();
            credit_maker(reward + pnl_delta);
            AccrualUpdate u;
            u.accrued_rewards = quote.accrued_rewards + reward;
            u.realized_bleed = quote.realized_bleed + bleed;
            u.fills = quote.fills + n_fills;
            u.last_mid = mid;
            u.last_accrued_at = unix_to_iso(now);
            u.inventory_pnl = quote.inventory_pnl + pnl_delta;
            if (!okc) {
                u.inventory = quote.inventory;
                db_.update_maker_quote_accrual(quote.id, u);
                results.push_back({{"quote", maker_quote_to_dict(quote)}, {"exit_failed", "drift_exit"},
                                   {"mid", mid}});
                continue;
            }
            u.inventory = 0.0;
            const MakerQuote final_q = db_.update_maker_quote_accrual(quote.id, u);
            exit_maker_quote(final_q, mid, "drift_exit", results, 0.0,
                             json{{"reward", reward}, {"inventory_pnl_delta", pnl_delta},
                                  {"share", share}, {"seconds", seconds}});
            continue;
        }

        // 4. RE-CENTER — mid 移动 >= recenter_ticks → 撤两腿 + 重挂 BUY-YES + BUY-NO。
        json submitted = json::array();
        const bool forced = (force_recenter != nullptr) && force_recenter->count(quote.token_id) != 0;
        if (forced || std::abs(mid - quote.last_mid) >= std::max(1, recenter_ticks) * quote.tick) {
            cancel_both();
            std::vector<maker::Order> orders = maker::compute_two_sided_quotes_yes_no(
                mid, quote.half_spread_c, quote.size, quote.tick, quote.max_spread_c, quote.token_id,
                no_token, 0.0);
            for (const auto& o : orders) {
                if (o.token_id.empty()) continue;
                submitted.push_back(submitter({{"action", "PLACE"}, {"token_id", o.token_id},
                                               {"side", o.side}, {"price", o.price}, {"size", o.size}}));
            }
        }

        credit_maker(reward + pnl_delta);
        AccrualUpdate u;
        u.accrued_rewards = quote.accrued_rewards + reward;
        u.realized_bleed = quote.realized_bleed + bleed;
        u.fills = quote.fills + n_fills;
        u.last_mid = mid;
        u.last_accrued_at = unix_to_iso(now);
        u.inventory = quote.inventory;  // 无成交 → 库存不变 (常为 0)
        u.inventory_pnl = quote.inventory_pnl + pnl_delta;
        const MakerQuote updated = db_.update_maker_quote_accrual(quote.id, u);
        results.push_back({{"quote", maker_quote_to_dict(updated)},
                           {"reward", round_to(reward, 6)},
                           {"inventory", round_to(quote.inventory, 4)},
                           {"inventory_pnl_delta", round_to(pnl_delta, 6)},
                           {"share", round_to(share, 6)},
                           {"seconds", round_to(seconds, 2)},
                           {"mid", mid},
                           {"submitted", submitted},
                           {"fills_applied", n_fills}});
    }
    record_equity();
    return results;
}

// ---- suggest_maker_half_spread ----

json Engine::suggest_maker_half_spread(const std::string& slug_or_id, const std::string& outcome_in,
                                       double cancel_efficiency, double poll_seconds) {
    require_account();
    Market market = api_.get_market(slug_or_id);
    const std::string outcome = validate_outcome(outcome_in, &market);
    if (!(cancel_efficiency >= 0.0 && cancel_efficiency <= 1.0))
        throw OrderRejectedError("cancel_efficiency must be in [0, 1]");
    if (poll_seconds <= 0.0) throw OrderRejectedError("poll_seconds must be > 0");

    auto pool = api_.get_reward_config(market.condition_id);
    if (!pool) throw OrderRejectedError(market.slug + " is not in the liquidity-rewards program");
    const std::string token_id = market.get_token_id(outcome);
    const OrderBook book = api_.get_order_book(token_id);
    const double mid = api_.get_midpoint(token_id);
    if (!(0.0 < mid && mid < 1.0)) throw OrderRejectedError("No valid midpoint to anchor the maker quote");

    const double existing_qmin = ob::book_inband_qmin(book, mid, pool->max_spread);
    std::vector<ob::PricePoint> history;
    try {
        history = api_.prices_history(token_id);
    } catch (...) {
        history.clear();
    }
    const double sigma_c = ob::realized_sigma_c_from_history(history, poll_seconds);
    const ob::OptimalHalfSpread rec = ob::optimal_half_spread(
        pool->daily, pool->max_spread, pool->min_size, pool->tick * 100.0, existing_qmin, sigma_c,
        86400.0 / poll_seconds, cancel_efficiency);

    return {{"half_spread_c", rec.half_spread_c},
            {"net_per_day", rec.net_per_day},
            {"reward_per_day", rec.reward_per_day},
            {"bleed_per_day", rec.bleed_per_day},
            {"share", rec.share},
            {"market_slug", market.slug},
            {"condition_id", market.condition_id},
            {"outcome", outcome},
            {"mid", mid},
            {"sigma_c", round_to(sigma_c, 4)},
            {"existing_qmin", round_to(existing_qmin, 4)},
            {"max_spread_c", pool->max_spread},
            {"min_size", pool->min_size},
            {"daily_rate", pool->daily},
            {"tick_c", round_to(pool->tick * 100.0, 4)},
            {"poll_seconds", poll_seconds},
            {"cancel_efficiency", cancel_efficiency}};
}

std::vector<json> Engine::get_maker_quotes() {
    require_account();
    std::vector<json> out;
    for (const auto& q : db_.get_active_maker_quotes()) out.push_back(maker_quote_to_dict(q));
    return out;
}

std::optional<json> Engine::cancel_maker_quote(int quote_id) {
    require_account();
    auto quote = db_.get_maker_quote(quote_id);
    if (!quote || quote->status != "active") return std::nullopt;
    db_.update_cash(get_account().cash + quote->committed_capital);
    auto updated = db_.cancel_maker_quote(quote_id);
    record_equity();
    return updated ? std::optional<json>(maker_quote_to_dict(*updated)) : std::nullopt;
}

json Engine::get_maker_summary() {
    require_account();
    const std::vector<MakerQuote> quotes = db_.get_all_maker_quotes();
    double committed = 0.0, reward_income = 0.0, inventory_pnl = 0.0, bleed = 0.0, open_inventory = 0.0;
    int active = 0;
    for (const auto& q : quotes) {
        if (q.status == "active") {
            committed += q.committed_capital;
            open_inventory += q.inventory;
            ++active;
        }
        reward_income += q.accrued_rewards;
        inventory_pnl += q.inventory_pnl;
        bleed += q.realized_bleed;
    }
    return {{"active_quotes", active},
            {"total_quotes", static_cast<int>(quotes.size())},
            {"committed_capital", committed},
            {"open_inventory", open_inventory},
            {"reward_income", reward_income},
            {"inventory_pnl", inventory_pnl},
            {"adverse_bleed", bleed},
            {"net_maker_pnl", reward_income + inventory_pnl}};
}

json maker_quote_to_dict(const MakerQuote& q) {
    return {{"id", q.id},
            {"market_slug", q.market_slug},
            {"market_condition_id", q.market_condition_id},
            {"outcome", q.outcome},
            {"token_id", q.token_id},
            {"complement_token_id", q.complement_token_id},
            {"size", q.size},
            {"half_spread_c", q.half_spread_c},
            {"max_spread_c", q.max_spread_c},
            {"min_size", q.min_size},
            {"daily_rate", q.daily_rate},
            {"tick", q.tick},
            {"cancel_efficiency", q.cancel_efficiency},
            {"max_inventory", q.max_inventory},
            {"skew_strength", q.skew_strength},
            {"inventory", q.inventory},
            {"inventory_pnl", q.inventory_pnl},
            {"entry_mid", q.entry_mid},
            {"committed_capital", q.committed_capital},
            {"accrued_rewards", q.accrued_rewards},
            {"realized_bleed", q.realized_bleed},
            {"net_pnl", q.accrued_rewards + q.inventory_pnl},
            {"fills", q.fills},
            {"status", q.status},
            {"last_mid", q.last_mid},
            {"created_at", q.created_at},
            {"last_accrued_at", q.last_accrued_at}};
}

}  // namespace pmm
