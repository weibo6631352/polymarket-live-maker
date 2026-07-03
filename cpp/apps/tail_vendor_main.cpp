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
//   ./tail-vendor                       # 扫描模式 dry: 打印本轮会做什么
//   ./tail-vendor orders.json           # sheet 模式 dry: 校验+打印人工清单
//   PM_TRADER_LIVE=1 TV_ARM=1 TV_TOTAL_USD=50 ./tail-vendor [orders.json]   # armed
// orders.json: [{"token_id":"...","price":0.97,"size":10,"note":"slug"}] (price = BUY NO 价)
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

// 下单前活盘复核: gamma/清单价可能陈旧; 若真实 NO ask <= 我们的价, GTC 会过价变 taker。
bool would_cross(pmm::PolymarketClient& client, const std::string& no_token, double no_price) {
    const auto book = client.get_order_book(no_token);
    double live_no_ask = 1.0;
    for (const auto& lvl : book.asks) live_no_ask = std::min(live_no_ask, lvl.price);
    return live_no_ask <= no_price + 1e-9;
}

// ---- sheet 模式: 精确执行人工清单 (不扫描/不撤补; 硬顶+活盘复核+监控与扫描模式同一套) ----
int run_sheet(const char* path, const pmm::tail::Config& cfg, bool armed,
              pmm::PolymarketClient& client, pmm::clob::ClobSubmitter* sub, std::ofstream& lf) {
    nlohmann::json j;
    {
        std::ifstream in(path);
        if (!in) { std::fprintf(stderr, "cannot open %s\n", path); return 2; }
        try { in >> j; } catch (...) { std::fprintf(stderr, "bad json\n"); return 2; }
    }
    const auto sheet = pmm::tail::parse_sheet(j);
    if (!sheet) { std::fprintf(stderr, "sheet parse failed (need [{token_id,price,size,note}])\n"); return 2; }
    if (const auto err = pmm::tail::validate_sheet(*sheet, cfg)) {
        std::fprintf(stderr, "sheet VETO: %s\n", err->c_str());
        return 3;
    }
    double notional = 0.0;
    for (const auto& e : *sheet) notional += e.no_price * e.size;
    jlog(lf, {{"ev", "sheet_start"}, {"n", sheet->size()}, {"notional", notional}, {"armed", armed}});
    if (!armed) {
        for (const auto& e : *sheet)
            jlog(lf, {{"ev", "dry_place"}, {"note", e.note}, {"no_price", e.no_price}, {"size", e.size}});
        return 0;
    }
    if (auto bal = sub->usdc_balance()) {
        jlog(lf, {{"ev", "balance"}, {"usdc", *bal}});
        if (*bal < notional) { jlog(lf, {{"ev", "abort"}, {"why", "balance < sheet notional"}}); return 1; }
    }
    (void)sub->poll_fills();  // prime cursor
    int rejects = 0;
    for (const auto& e : *sheet) {
        if (would_cross(client, e.no_token, e.no_price)) {
            jlog(lf, {{"ev", "stale_quote_skip"}, {"note", e.note}, {"our_no", e.no_price}});
            continue;
        }
        sub->warm_token(e.no_token);
        const json r = (*sub)({{"action", "PLACE"}, {"token_id", e.no_token}, {"side", "BUY"},
                               {"price", e.no_price}, {"size", e.size}});
        jlog(lf, {{"ev", "place"}, {"note", e.note}, {"no_price", e.no_price}, {"size", e.size},
                  {"resp", r}});
        if (r.value("status", "") != "PLACED" && ++rejects >= 3) {
            jlog(lf, {{"ev", "halt"}, {"why", "3 rejects in sheet — investigate"}});
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }
    // 监控: 成交/急停/心跳 (挂单驻留由 CLOB 保持; 退出不撤单, STOP 文件才撤)。
    long long tick = 0;
    while (g_run.load()) {
        for (int i = 0; i < 60 && g_run.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (std::ifstream("STOP_TAIL_VENDOR").good()) break;
        }
        if (std::ifstream("STOP_TAIL_VENDOR").good()) {
            jlog(lf, {{"ev", "stopfile"}});
            for (const auto& e : *sheet)
                jlog(lf, {{"ev", "cancel"}, {"why", "stopfile"},
                          {"resp", (*sub)({{"action", "CANCEL_ALL"}, {"token_id", e.no_token}})}});
            break;
        }
        for (const auto& f : sub->poll_fills()) jlog(lf, {{"ev", "fill"}, {"fill", f}});
        if (++tick % 60 == 0) {
            json bal;
            if (auto b = sub->usdc_balance()) bal = *b;
            jlog(lf, {{"ev", "status"}, {"open", sub->list_open_orders().size()}, {"usdc", bal}});
        }
    }
    jlog(lf, {{"ev", "exit"}, {"note", "sheet orders remain resting; rerun to resume monitoring"}});
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
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
              {"max_orders", cfg.max_orders}, {"scan_s", scan_s},
              {"mode", argc > 1 ? "sheet" : "scan"}});

    if (argc > 1) return run_sheet(argv[1], cfg, armed, client, sub.get(), lf);

    // 已成交持有的 NO 抵押 (保守按 $1/股计入; 运行中由 poll_fills 增量累加 — 持久, 不随轮清零)。
    double held_total = 0.0;
    std::map<std::string, double> held_coin;
    if (const char* funder = std::getenv("TV_FUNDER")) {
        for (const auto& [tok, sz] : client.chain_positions(funder)) held_total += std::abs(sz);
        jlog(lf, {{"ev", "chain_reconcile"}, {"held_shares_as_usd", held_total}});
    }
    if (armed) (void)sub->poll_fills();  // prime cursor

    std::map<std::string, std::string> token_coin;  // no_token -> coin (fill 归因用)
    std::map<std::string, std::string> token_note;
    int consecutive_rejects = 0;
    long long tick_count = 0;

    while (g_run.load()) {
        const double now = static_cast<double>(std::time(nullptr));

        // ---- STOP 急停 (轮首; 睡眠中每秒也查 -> 最长 1s 响应) ----
        if (std::ifstream("STOP_TAIL_VENDOR").good()) {
            jlog(lf, {{"ev", "stopfile"}});
            if (armed)
                for (const auto& oo : sub->list_open_orders())
                    jlog(lf, {{"ev", "cancel"}, {"why", "stopfile"},
                              {"resp", (*sub)({{"action", "CANCEL_ALL"},
                                               {"token_id", oo.value("asset_id", "")}})}});
            break;
        }

        // ---- 1) 按 series_id 精确扫描 (校准过的可交易家族; 每轮 ~11 请求, 无微市场/体育洪水) ----
        // strike 家族 5.3x: 45/42=BTC/ETH daily, 10022/10023/10024=SOL/XRP/其它 daily,
        // 10147/10149=monthly; negrisk 家族 1.9x: 10041/10065/10107/10247。touch 家族定价公允, 不扫。
        static const char* kSeries[] = {"45", "42", "10022", "10023", "10024",
                                        "10147", "10149",
                                        "10041", "10065", "10107", "10247"};
        std::map<std::string, tail::Candidate> cands;  // no_token -> cand
        for (const char* sid : kSeries) {
            const json evs = client.gamma_events_raw(
                {{"series_id", sid}, {"closed", "false"}, {"limit", "100"}});
            if (!evs.is_array()) continue;
            for (const auto& ev : evs) {
                const auto mk = ev.find("markets");
                if (mk == ev.end() || !mk->is_array()) continue;
                for (const auto& r : *mk)
                    if (auto c = tail::parse_candidate(r, now)) cands[c->no_token] = *c;
            }
        }

        // ---- 2) 成交增量 -> 持久 held 记账 ($1/股保守); 在场挂单快照 ----
        std::vector<tail::OpenOrder> open;
        if (armed) {
            for (const auto& f : sub->poll_fills()) {
                jlog(lf, {{"ev", "fill"}, {"fill", f}});
                const std::string tok = f.value("token_id", "");
                const double sz = f.value("size", 0.0);
                held_total += sz;
                if (auto it = token_coin.find(tok); it != token_coin.end()) held_coin[it->second] += sz;
            }
            for (const auto& oo : sub->list_open_orders()) {
                tail::OpenOrder o;
                o.no_token = oo.value("asset_id", "");
                o.no_price = std::atof(oo.value("price", "0").c_str());
                o.size = std::atof(oo.value("original_size", "0").c_str()) -
                         std::atof(oo.value("size_matched", "0").c_str());
                if (auto it = token_coin.find(o.no_token); it != token_coin.end()) o.coin = it->second;
                else if (auto ic = cands.find(o.no_token); ic != cands.end()) o.coin = ic->second.coin;
                if (auto in = token_note.find(o.no_token); in != token_note.end()) o.note = in->second;
                open.push_back(std::move(o));
            }
        }
        for (const auto& [tok, c] : cands) {  // fill 归因表随候选集更新
            token_coin[tok] = c.coin;
            token_note[tok] = c.slug;
        }

        // ---- 3) 决策 (纯函数) -> 执行 ----
        const auto actions = tail::plan(cands, open, held_coin, held_total, cfg);
        int executed = 0;
        for (const auto& a : actions) {
            if (!g_run.load()) break;
            if (a.kind == tail::Action::Kind::kCancel) {
                if (armed) {
                    const json r = (*sub)({{"action", "CANCEL_ALL"}, {"token_id", a.no_token}});
                    jlog(lf, {{"ev", "cancel"}, {"why", a.why}, {"note", a.note}, {"resp", r}});
                } else {
                    jlog(lf, {{"ev", "dry_cancel"}, {"why", a.why}, {"note", a.note}});
                }
                ++executed;
                continue;
            }
            if (!armed) {
                jlog(lf, {{"ev", "dry_place"}, {"note", a.note}, {"no_price", a.no_price},
                          {"size", a.size}, {"sell_yes_at", 1.0 - a.no_price}});
                continue;
            }
            if (would_cross(client, a.no_token, a.no_price)) {  // 陈旧报价防交叉 (详见 helper)
                jlog(lf, {{"ev", "stale_quote_skip"}, {"note", a.note}, {"our_no", a.no_price}});
                continue;
            }
            sub->warm_token(a.no_token);
            const json r = (*sub)({{"action", "PLACE"}, {"token_id", a.no_token}, {"side", "BUY"},
                                   {"price", a.no_price}, {"size", a.size}});
            jlog(lf, {{"ev", "place"}, {"note", a.note}, {"no_price", a.no_price},
                      {"size", a.size}, {"resp", r}});
            if (r.value("status", "") == "PLACED") {
                consecutive_rejects = 0;
            } else if (++consecutive_rejects >= 3) {
                jlog(lf, {{"ev", "halt"}, {"why", "3 consecutive rejects — investigate"}});
                g_run.store(false);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            ++executed;
        }

        // ---- 5) 心跳/权益地板 ----
        // 每轮心跳 (事件驱动日志与死机不可区分 — 观测性要求每轮走表); 每 ~1h 附带余额。
        json hb = {{"ev", "scan"}, {"candidates", cands.size()}, {"open", open.size()},
                   {"held_usd", held_total}, {"planned", actions.size()}, {"executed", executed}};
        if (tick_count % 12 == 0 && armed)
            if (auto b = sub->usdc_balance()) hb["usdc"] = *b;
        jlog(lf, hb);
        ++tick_count;
        for (int i = 0; i < scan_s && g_run.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (std::ifstream("STOP_TAIL_VENDOR").good()) break;  // 1s 级急停响应
        }
    }
    jlog(lf, {{"ev", "exit"}, {"note", "resting GTC orders remain by design; restart resumes via reconcile"}});
    return 0;
}
