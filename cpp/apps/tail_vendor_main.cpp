// apps/tail_vendor_main.cpp — 常驻系统性上行尾卖方 (tail-seller 的泛化)。
//
// 循环 (慢频, 默认 300s): 扫 gamma 窗口 -> parse_candidate/decide (纯函数核) -> 与在场挂单对账
// (撤带外/逼近障碍/被压价的, 补新候选), GTC BUY NO, 成交即持有到期。性能 = PersistentHttps 连接
// 复用 + 每轮 ~10-30 个请求 + 无忙等; 无 SQLite 状态 (重启从 list_open_orders + 链上持仓对账)。
//
// 安全 (同 tail-seller): 默认 DRY; 武装 = PM_TRADER_LIVE=1 且 TV_ARM=1; 编译期硬顶 (env 只能调低):
// 总抵押 $250 / 单币 $100 / 单笔 $25 / 挂单数 24; STOP_TAIL_VENDOR 文件急停 (撤全部+退出);
// 连续 3 次下单被拒 -> 自动停机保全现场。
//
//   ./tail-vendor                # dry: 打印本轮会做什么
//   PM_TRADER_LIVE=1 TV_ARM=1 TV_TOTAL_USD=50 ./tail-vendor    # 试验档
// env: TV_TOTAL_USD TV_PER_COIN_USD TV_PER_ORDER_USD TV_MAX_ORDERS TV_SCAN_S TV_FUNDER
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "pmm/api.hpp"
#include "pmm/app/dotenv.hpp"
#include "pmm/clob_submitter.hpp"
#include "pmm/db.hpp"
#include "pmm/tail_vendor.hpp"

using nlohmann::json;
namespace tail = pmm::tail;

namespace {
// 编译期硬顶 — env 只能调低, 不能调高。
constexpr double kCeilTotalUsd = 250.0;
constexpr double kCeilPerCoinUsd = 100.0;
constexpr double kCeilPerOrderUsd = 25.0;
constexpr int kCeilOrders = 24;

std::atomic<bool> g_run{true};
void on_sig(int) { g_run.store(false); }

long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void jlog(std::ofstream& f, json j) {
    j["t_ms"] = now_ms();
    f << j.dump() << "\n";
    f.flush();
    std::printf("%s\n", j.dump().c_str());
}

double env_low(const char* name, double dflt, double ceil) {
    if (const char* s = std::getenv(name)) return std::min(std::atof(s), ceil);
    return std::min(dflt, ceil);
}

struct OurOrder {
    double no_price{0};
    double size{0};
    std::string coin;
    std::string note;
};
}  // namespace

int main() {
    tail::Config cfg;
    cfg.total_usd = env_low("TV_TOTAL_USD", 60.0, kCeilTotalUsd);
    cfg.per_coin_usd = env_low("TV_PER_COIN_USD", 60.0, kCeilPerCoinUsd);
    cfg.per_order_usd = env_low("TV_PER_ORDER_USD", 20.0, kCeilPerOrderUsd);
    cfg.max_orders = static_cast<int>(env_low("TV_MAX_ORDERS", 8, kCeilOrders));
    const int scan_s = static_cast<int>(env_low("TV_SCAN_S", 300, 3600));

    const char* lv = std::getenv("PM_TRADER_LIVE");
    const char* av = std::getenv("TV_ARM");
    const bool armed = lv && std::string(lv) == "1" && av && std::string(av) == "1";

    std::signal(SIGINT, on_sig);
    std::signal(SIGTERM, on_sig);
    pmm::app::LoadDotEnv(".env", /*live_mode=*/armed);

    pmm::Database db("state");
    pmm::PolymarketClient client(db);
    std::unique_ptr<pmm::clob::ClobSubmitter> sub;
    if (armed) {
        sub = std::make_unique<pmm::clob::ClobSubmitter>();
        if (!sub->ready()) {
            std::fprintf(stderr, "ClobSubmitter NOT ready (key/creds)\n");
            return 1;
        }
    }
    std::ofstream lf("tail_vendor_log.jsonl", std::ios::app);
    jlog(lf, {{"ev", "start"}, {"armed", armed}, {"total_usd", cfg.total_usd},
              {"per_coin_usd", cfg.per_coin_usd}, {"per_order_usd", cfg.per_order_usd},
              {"max_orders", cfg.max_orders}, {"scan_s", scan_s}});

    // 链上已持有的 NO 抵押 (保守按 $1/股计入总上限; 老仓无法归因币种 -> 只计 total)。
    double held_total = 0.0;
    if (const char* funder = std::getenv("TV_FUNDER")) {
        for (const auto& [tok, sz] : client.chain_positions(funder)) held_total += std::abs(sz);
        jlog(lf, {{"ev", "chain_reconcile"}, {"held_shares_as_usd", held_total}});
    }
    if (armed) (void)sub->poll_fills();  // prime cursor

    std::map<std::string, OurOrder> ours;  // no_token -> order (重启时从 list_open_orders 重建)
    int consecutive_rejects = 0;
    long long tick_count = 0;

    while (g_run.load()) {
        const double now = static_cast<double>(std::time(nullptr));

        // ---- STOP 急停 ----
        if (std::ifstream("STOP_TAIL_VENDOR").good()) {
            jlog(lf, {{"ev", "stopfile"}});
            if (armed)
                for (const auto& [tok, o] : ours)
                    jlog(lf, {{"ev", "cancel"}, {"why", "stopfile"},
                              {"resp", (*sub)({{"action", "CANCEL_ALL"}, {"token_id", tok}})}});
            break;
        }

        // ---- 1) 扫描窗口 (endDate ∈ [now, now+days_max]) ----
        std::map<std::string, tail::Candidate> cands;  // no_token -> cand
        {
            char lo[32], hi[32];
            // 窗口下沿 now+6h: 跳过即将到期的微市场洪流, 且持单市场在 pull 阈值 (days_min/2=12h)
            // 前始终可见; tag_id=21 (crypto) 服务端过滤 — 否则体育盘淹没 offset 上限。
            std::time_t tlo = static_cast<std::time_t>(now + 6 * 3600);
            std::time_t thi = static_cast<std::time_t>(now + cfg.days_max * 86400);
            std::strftime(lo, sizeof lo, "%Y-%m-%dT%H:%M:%SZ", gmtime(&tlo));
            std::strftime(hi, sizeof hi, "%Y-%m-%dT%H:%M:%SZ", gmtime(&thi));
            for (int off = 0; off < 4000; off += 100) {
                const json rows = client.gamma_markets_raw(
                    {{"closed", "false"}, {"limit", "100"}, {"offset", std::to_string(off)},
                     {"order", "endDate"}, {"ascending", "true"}, {"tag_id", "21"},
                     {"end_date_min", lo}, {"end_date_max", hi}});
                if (!rows.is_array() || rows.empty()) break;
                for (const auto& r : rows)
                    if (auto c = tail::parse_candidate(r, now)) cands[c->no_token] = *c;
                if (rows.size() < 100) break;
            }
        }

        // ---- 2) 成交轮询 + 在场挂单重建 (armed) ----
        double resting_total = 0.0, filled_total = 0.0;
        std::map<std::string, double> deployed_coin;
        if (armed) {
            for (const auto& f : sub->poll_fills()) {
                jlog(lf, {{"ev", "fill"}, {"fill", f}});
                const std::string tok = f.value("token_id", "");
                const double sz = f.value("size", 0.0);
                filled_total += sz;  // 保守 $1/股
                if (auto it = ours.find(tok); it != ours.end()) deployed_coin[it->second.coin] += sz;
            }
            std::map<std::string, OurOrder> rebuilt;
            for (const auto& oo : sub->list_open_orders()) {
                const std::string tok = oo.value("asset_id", "");
                const double px = std::atof(oo.value("price", "0").c_str());
                const double sz = std::atof(oo.value("original_size", "0").c_str()) -
                                  std::atof(oo.value("size_matched", "0").c_str());
                OurOrder o{px, sz, "", ""};
                if (auto it = ours.find(tok); it != ours.end()) { o.coin = it->second.coin; o.note = it->second.note; }
                else if (auto ic = cands.find(tok); ic != cands.end()) { o.coin = ic->second.coin; o.note = ic->second.slug; }
                rebuilt[tok] = o;
                resting_total += px * sz;
                if (!o.coin.empty()) deployed_coin[o.coin] += px * sz;
            }
            ours = std::move(rebuilt);
        }
        const double deployed_total = resting_total + filled_total + held_total;

        // ---- 3) 持单管理: 带外/逼近障碍/被压价 -> 撤 ----
        int actions = 0;
        for (auto it = ours.begin(); it != ours.end();) {
            const std::string& tok = it->first;
            auto ic = cands.find(tok);
            const bool gone = (ic == cands.end());
            const bool pull = !gone && tail::should_pull(ic->second, cfg);
            const bool outbid = !gone && (ic->second.yes_ask < (1.0 - it->second.no_price) - cfg.tick - 1e-9);
            if (gone || pull || outbid) {
                const char* why = gone ? "left_window" : (pull ? "pull_signal" : "outbid");
                if (armed) {
                    const json r = (*sub)({{"action", "CANCEL_ALL"}, {"token_id", tok}});
                    jlog(lf, {{"ev", "cancel"}, {"why", why}, {"note", it->second.note}, {"resp", r}});
                } else {
                    jlog(lf, {{"ev", "dry_cancel"}, {"why", why}, {"note", it->second.note}});
                }
                it = ours.erase(it);
                ++actions;
            } else {
                ++it;
            }
        }

        // ---- 4) 新报单 ----
        for (const auto& [tok, c] : cands) {
            if (ours.count(tok)) continue;
            if (static_cast<int>(ours.size()) >= cfg.max_orders) break;
            const auto q = tail::decide(c, cfg, deployed_coin[c.coin], deployed_total);
            if (!q) continue;
            if (!armed) {
                jlog(lf, {{"ev", "dry_place"}, {"note", q->note}, {"no_price", q->no_price},
                          {"size", q->size}, {"sell_yes_at", 1.0 - q->no_price}});
                continue;
            }
            sub->warm_token(tok);
            const json r = (*sub)({{"action", "PLACE"}, {"token_id", tok}, {"side", "BUY"},
                                   {"price", q->no_price}, {"size", q->size}});
            jlog(lf, {{"ev", "place"}, {"note", q->note}, {"no_price", q->no_price},
                      {"size", q->size}, {"resp", r}});
            if (r.value("status", "") == "PLACED") {
                ours[tok] = {q->no_price, q->size, c.coin, c.slug};
                deployed_coin[c.coin] += q->no_price * q->size;
                consecutive_rejects = 0;
            } else if (++consecutive_rejects >= 3) {
                jlog(lf, {{"ev", "halt"}, {"why", "3 consecutive rejects — investigate"}});
                g_run.store(false);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            ++actions;
        }

        // ---- 5) 心跳/权益地板 ----
        if (tick_count % 12 == 0) {  // 每 ~1h (scan_s=300)
            json bal;
            if (armed)
                if (auto b = sub->usdc_balance()) bal = *b;
            jlog(lf, {{"ev", "status"}, {"candidates", cands.size()}, {"ours", ours.size()},
                      {"deployed_total", deployed_total}, {"usdc", bal}, {"actions", actions}});
        }
        ++tick_count;
        for (int i = 0; i < scan_s && g_run.load(); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    jlog(lf, {{"ev", "exit"}, {"note", "resting GTC orders remain by design; restart resumes via reconcile"}});
    return 0;
}
