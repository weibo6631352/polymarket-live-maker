// apps/tail_vendor_main.cpp — 常驻系统性上行尾卖方 (tail-seller 的泛化)。
//
// 循环 (慢频, 默认 300s): 扫 gamma 窗口 -> parse_candidate/decide (纯函数核) -> 与在场挂单对账
// (撤带外/逼近障碍/被压价的, 补新候选), GTC BUY NO, 成交即持有到期。性能 = PersistentHttps 连接
// 复用 + 每轮 ~10-30 个请求 + 无忙等; 无 SQLite 状态 (重启从 list_open_orders + 链上持仓对账)。
//
// 安全 (同 tail-seller): 默认 DRY; 武装 = PM_TRADER_LIVE=1 且 TV_ARM=1; 编译期硬顶 (env 只能调低):
// 总抵押 $500 / 单币 $200 / 单笔 $25 / 挂单数 60 (2026-07-07 扩容, 用户授权); STOP_TAIL_VENDOR 急停;
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
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
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
constexpr double kCeilTotalUsd = 500.0;    // 2026-07-07 扩容 (用户授权; env 只能调低于此)
constexpr double kCeilPerCoinUsd = 200.0;
constexpr double kCeilPerOrderUsd = 25.0;
constexpr double kCeilPerMarketUsd = 50.0;
constexpr int kCeilOrders = 60;
// 触碰/reach 卫星腿的执行硬顶 (小; env 只能调低)。默认预算 0 = 触碰执行 OFF, 需显式 TV_TOUCH_TOTAL_USD>0 武装。
constexpr double kCeilTouchTotalUsd = 120.0;   // 2026-07-09 触碰腿首试 (用户选 $80 总)
constexpr double kCeilTouchPerCoinUsd = 60.0;
constexpr double kCeilTouchOrderUsd = 25.0;    // 单触碰仓 (curator 风险平价送 base$12/cap$24)
// 卖价带上限硬顶 (2026-07-10 抬顶追流量; env 只能低于此)。yes_exit 硬顶须 > yes_max 硬顶 (卖后不立即退出)。
constexpr double kCeilYesMax = 0.12;
constexpr double kCeilYesExit = 0.20;

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
    std::fflush(stdout);  // journald 管道是块缓冲 — 不冲则日志滞留到退出, 心跳观测性归零
}

// ---- 成交记账持久化: 重启/崩溃后 held 抵押 + fill 游标不丢 (资金硬顶跨进程生命周期有效) ----
struct HeldState {
    double total{0};                                    // 已成交持有的抵押 ($1/股保守)
    std::map<std::string, double> coin;                 // 币种 -> 抵押
    std::map<std::string, double> token_held;           // no_token -> 抵押 (单市场集中度刹车)
    std::map<std::string, std::string> token_coin;      // 下过单的 no_token -> coin (fill 归因)
    std::map<std::string, std::string> token_note;
    std::map<std::string, bool> token_touch;            // no_token -> 是否触碰腿 (held 拆分 core/触碰预算)
    std::string last_trade_id;                          // 已入账的最新成交 id (重启补账基准)
};
constexpr const char* kHeldPath = "state/tail_vendor_held.json";

HeldState load_held(std::ofstream& lf) {
    HeldState st;
    std::ifstream in(kHeldPath);
    if (!in) return st;
    try {
        json j;
        in >> j;
        st.total = j.value("held_total", 0.0);
        // range-for 不给 .items() 里层的临时续命 (悬垂 UB) — 先落成具名对象再迭代。
        const json jc = j.value("held_coin", json::object());
        for (const auto& [k, v] : jc.items()) st.coin[k] = v.get<double>();
        const json jth = j.value("token_held", json::object());
        for (const auto& [k, v] : jth.items()) st.token_held[k] = v.get<double>();
        const json jtc = j.value("token_coin", json::object());
        for (const auto& [k, v] : jtc.items()) st.token_coin[k] = v.get<std::string>();
        const json jtn = j.value("token_note", json::object());
        for (const auto& [k, v] : jtn.items()) st.token_note[k] = v.get<std::string>();
        const json jtt = j.value("token_touch", json::object());
        for (const auto& [k, v] : jtt.items())
            if (v.is_boolean()) st.token_touch[k] = v.get<bool>();  // 坏条目跳过, 不抛 (否则整表被清空 → 触碰 held 低估 → 超 $80)
        st.last_trade_id = j.value("last_trade_id", "");
    } catch (...) {
        // 坏状态文件 = held 低估风险 (上限可能被突破) — 大声报, 人来查
        jlog(lf, {{"ev", "error"}, {"what", "held_state_corrupt — held undercounts, investigate"}});
    }
    return st;
}

void save_held(const HeldState& st, std::ofstream& lf) {
    std::error_code ec;
    std::filesystem::create_directories("state", ec);
    const json j{{"held_total", st.total},         {"held_coin", st.coin},
                 {"token_coin", st.token_coin},    {"token_note", st.token_note},
                 {"token_touch", st.token_touch},  {"token_held", st.token_held},
                 {"last_trade_id", st.last_trade_id}};
    const std::string tmp = std::string(kHeldPath) + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << j.dump();
    }
    std::filesystem::rename(tmp, kHeldPath, ec);
    if (ec) jlog(lf, {{"ev", "error"}, {"what", "held_state_save"}, {"code", ec.value()}});
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
    // (fill 游标由 ClobSubmitter 构造时 prime; 这里再 prime 会吞掉构造→此处之间旧挂单的成交)
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
    cfg.per_market_usd = env_low("TV_PER_MARKET_USD", 15.0, kCeilPerMarketUsd);
    cfg.max_orders = static_cast<int>(env_low("TV_MAX_ORDERS", 8, kCeilOrders));
    // 触碰腿执行预算 (默认 0 = OFF; 首试 arm 用 TV_TOUCH_TOTAL_USD=80 TV_TOUCH_PER_COIN_USD=48)。
    // 与 curation 侧的 TV_TOUCH=1 双门: 白名单出触碰 slug 且此处预算>0 才会真下触碰单。
    cfg.touch_total_usd = env_low("TV_TOUCH_TOTAL_USD", 0.0, kCeilTouchTotalUsd);
    cfg.touch_per_coin_usd = env_low("TV_TOUCH_PER_COIN_USD", 0.0, kCeilTouchPerCoinUsd);
    cfg.touch_per_order_usd = env_low("TV_TOUCH_PER_ORDER_USD", 24.0, kCeilTouchOrderUsd);
    cfg.touch_per_market_usd = cfg.touch_per_order_usd;  // 一个 reach 行权价 = 一个市场
    // 卖价带上限 (2026-07-10): 回测显示 7-10c 尾仍 +EV, 而散户流量移到旧 7c 带顶之外 -> 抬顶追流量。
    // 默认保持旧值 0.07/0.10 (重编不改行为); 抬顶=显式 drop-in TV_YES_MAX=0.10 TV_YES_EXIT=0.15 (可秒回退)。
    // 风险由 Deribit fair 过滤 (BTC/ETH) + regime gate (SOL/XRP) 兜底。yes_exit 必须 > yes_max。
    cfg.yes_max = env_low("TV_YES_MAX", 0.07, kCeilYesMax);
    cfg.yes_exit = env_low("TV_YES_EXIT", 0.10, kCeilYesExit);
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
              {"yes_max", cfg.yes_max}, {"yes_exit", cfg.yes_exit},
              {"touch_total_usd", cfg.touch_total_usd},
              {"mode", argc > 1 ? "sheet" : "scan"}});

    if (argc > 1) return run_sheet(argv[1], cfg, armed, client, sub.get(), lf);

    // 已成交持有的 NO 抵押 (保守按 $1/股计入) — 持久账本, 重启/崩溃不清零 (资金硬顶的前提)。
    HeldState st = load_held(lf);
    jlog(lf, {{"ev", "held_reconcile"}, {"held_usd", st.total}, {"held_coin", st.coin},
              {"last_trade_id", st.last_trade_id}, {"tokens", st.token_coin.size()}});
    // ---- held 链上校正 (2026-07-10 根因修复) ----
    // 根因: ingest_fill 只在成交时 +held, 结算/赎回时不减 -> held 单调虚增 -> 数日后 BTC/ETH 虚满 per_coin
    // 上限 -> 拒下新单 -> 成交枯竭 (实测 BTC held $147 vs 链上真实 $78, 近2倍)。修法: 用链上真实持仓
    // (data-api /positions, $1/股) 重建 held, 只作用于本策略下过的 token (token_held 键, 避开共享账户
    // pmm-maker 的仓)。已赎回的仓链上 size≈0 -> 移除 -> 释放其额度。
    // FAIL-SAFE: funder 未设 / 拉取空或过少 (<3 仓, 疑似 API 抖动) -> 不动 held (保守用旧值, 绝不因残缺
    // 拉取虚减 held 导致超铺)。
    const std::string funder = std::getenv("TV_FUNDER") ? std::getenv("TV_FUNDER") : "";
    auto reconcile_held = [&]() {
        if (funder.empty() || st.token_held.empty()) return;
        const auto cp = client.chain_positions(funder);   // token -> 链上真实 size
        if (cp.size() < 3) return;                         // 空/过少 = 疑似残缺拉取 -> 跳过 (保守)
        std::map<std::string, double> new_coin, new_th;
        double new_total = 0.0;
        for (const auto& [tok, sz] : st.token_held) {
            const auto it = cp.find(tok);
            const double real = (it != cp.end()) ? std::abs(it->second) : 0.0;  // <0.5 = 已赎回/清仓
            if (real < 0.5) continue;                      // 仓已消失 -> 从 held 移除, 释放额度
            new_th[tok] = real;
            new_total += real;
            if (const auto ci = st.token_coin.find(tok); ci != st.token_coin.end())
                new_coin[ci->second] += real;
        }
        const double freed = st.total - new_total;
        if (std::abs(freed) < 0.5) return;                 // 无变化则不写盘
        jlog(lf, {{"ev", "held_chain_reconcile"}, {"old_held", st.total}, {"new_held", new_total},
                  {"freed", freed}, {"coin", new_coin}});
        st.token_held = std::move(new_th);
        st.coin = std::move(new_coin);
        st.total = new_total;
        save_held(st, lf);
    };
    reconcile_held();  // 启动即校正一次

    // 入账一笔成交: held 累加 + 游标推进 + 落盘。调用方必须按 oldest→newest 喂 (游标停在最新)。
    // 只认本策略下过单的 token (token_coin 注册表) — 同账户还有别的策略在跑 (pmm temp maker),
    // 它们的成交若入本账会虚耗 total 硬顶。外来成交记 fill_foreign, 游标照推。
    auto ingest_fill = [&](const json& f, const char* ev) {
        if (const std::string id = f.value("id", ""); !id.empty()) st.last_trade_id = id;
        const auto it = st.token_coin.find(f.value("token_id", ""));
        if (it == st.token_coin.end()) {
            jlog(lf, {{"ev", "fill_foreign"}, {"fill", f}});
            save_held(st, lf);
            return;
        }
        jlog(lf, {{"ev", ev}, {"fill", f}});
        if (f.value("side", "") == "BUY") {  // 我们只 BUY NO; 非 BUY = 人工干预, 只记日志
            const double sz = f.value("size", 0.0);
            st.total += sz;
            st.coin[it->second] += sz;
            st.token_held[f.value("token_id", "")] += sz;
        }
        save_held(st, lf);
    };
    if (armed && !st.last_trade_id.empty()) {  // 宕机期间的成交补账 (newest-first → 反着喂)
        const auto missed = sub->fills_since(st.last_trade_id);
        for (auto it = missed.rbegin(); it != missed.rend(); ++it) ingest_fill(*it, "fill_recovered");
    }

    int consecutive_rejects = 0;   // 业务拒单 (http 4xx) 连计数 -> 3 连 halt 保全现场
    int consecutive_neterr = 0;    // 传输层/5xx 瞬时故障连计数 -> 退避, 绝不 halt
    long long tick_count = 0;

    while (g_run.load()) {
        try {
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

        // ---- 0.5) held 链上校正 (每轮): 移除已结算/赎回的仓 -> 释放虚占的 per_coin/total 额度。 ----
        if (armed) reconcile_held();

        // ---- 1) 按 series_id 精确扫描 (校准过的可交易家族; 每轮 ~15 请求, 无微市场/体育洪水) ----
        // strike 家族 5.3x: 45/42=BTC/ETH daily, 10022/10023/10024=SOL/XRP/其它 daily,
        // 10147/10149=monthly; negrisk 家族 1.9x: 10041/10065/10107/10247。
        // touch/reach 家族 (2026-07-10 触碰腿; 周盘 "what-price-will-X-hit-july-6-12"): 10151/10152/10170/10239
        // = BTC/ETH/SOL/XRP。TV_TOUCH 关时它们进 cands 但不在白名单 -> 被过滤, 无害。
        static const char* kSeries[] = {"45", "42", "10022", "10023", "10024",
                                        "10147", "10149",
                                        "10041", "10065", "10107", "10247",
                                        "10151", "10152", "10170", "10239"};
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

        // ---- 1.5) 会话策展白名单 (LLM 端产出 state/tail_vendor_whitelist.json, 每轮热读) ----
        // {"slugs": [...]}: 非空 → 只做名单内市场; 名单外的在场挂单变 left_window → 撤 → 资本轮换。
        // 名单全不匹配 = 全撤且不下新单 (安全方向); 坏文件/空名单 = 忽略 (不限制)。
        // 策展白名单是唯一的交易授权来源 — FAIL-CLOSED: 文件缺失/损坏/空名单 → 本轮不交易
        // (candidates 清空; 在场挂单会被 left_window 撤掉 = 安全方向)。绝不退化为无策展全宇宙交易。
        std::size_t wl_size = 0;
        {
            bool authorized = false;
            std::ifstream wf("state/tail_vendor_whitelist.json");
            if (wf.good()) {
                try {
                    json wj;
                    wf >> wj;
                    std::set<std::string> allow;
                    for (const auto& s : wj.value("slugs", json::array()))
                        allow.insert(s.get<std::string>());
                    std::set<std::string> clamp;
                    for (const auto& s : wj.value("clamp", json::array()))
                        clamp.insert(s.get<std::string>());
                    // detail[] 里携带触碰腿的 anchor + per-row collateral (curator 风险平价)。
                    // slug -> collateral; 只有 anchor==touch-model 的行进触碰腿 (独立预算+sizing)。
                    // 全程类型守卫: 任何畸形 detail 行只跳过, 绝不抛异常 —— 否则会连累 core 授权 fail-closed
                    // (authorized 停在 false → cands.clear() → core 书被撤)。触碰的 curator 笔误不许拖垮 core。
                    std::map<std::string, double> touch_coll;
                    if (const auto jd = wj.find("detail"); jd != wj.end() && jd->is_array()) {
                        for (const auto& d : *jd) {
                            if (!d.is_object()) continue;
                            const auto ja = d.find("anchor");
                            if (ja == d.end() || !ja->is_string() || ja->get<std::string>() != "touch-model")
                                continue;
                            const auto js = d.find("slug");
                            if (js == d.end() || !js->is_string()) continue;
                            const auto jc = d.find("collateral");
                            touch_coll[js->get<std::string>()] =
                                (jc != d.end() && jc->is_number()) ? jc->get<double>() : 0.0;
                        }
                    }
                    if (!allow.empty()) {
                        authorized = true;
                        wl_size = allow.size();
                        for (auto it = cands.begin(); it != cands.end();) {
                            if (allow.count(it->second.slug) == 0) { it = cands.erase(it); continue; }
                            it->second.band_clamp = clamp.count(it->second.slug) != 0;
                            if (const auto tc = touch_coll.find(it->second.slug); tc != touch_coll.end()) {
                                it->second.is_touch = true;
                                it->second.wl_collateral = tc->second;
                            }
                            ++it;
                        }
                    }
                } catch (...) {
                }
            }
            if (!authorized) {
                cands.clear();
                jlog(lf, {{"ev", "error"},
                          {"what", "whitelist missing/corrupt/empty — FAIL-CLOSED, no trading this scan"}});
            }
        }

        // ---- 2) 成交增量 -> 持久 held 记账 ($1/股保守); 在场挂单快照 ----
        std::vector<tail::OpenOrder> open;
        if (armed) {
            const auto fills = sub->poll_fills();  // newest-first → 反着入账 (游标停在最新)
            for (auto it = fills.rbegin(); it != fills.rend(); ++it) ingest_fill(*it, "fill");
            for (const auto& oo : sub->list_open_orders()) {
                tail::OpenOrder o;
                o.no_token = oo.value("asset_id", "");
                o.no_price = std::atof(oo.value("price", "0").c_str());
                o.size = std::atof(oo.value("original_size", "0").c_str()) -
                         std::atof(oo.value("size_matched", "0").c_str());
                if (auto it = st.token_coin.find(o.no_token); it != st.token_coin.end())
                    o.coin = it->second;
                else if (auto ic = cands.find(o.no_token); ic != cands.end()) o.coin = ic->second.coin;
                if (auto in = st.token_note.find(o.no_token); in != st.token_note.end())
                    o.note = in->second;
                open.push_back(std::move(o));
            }
        }

        // ---- 2.5) 白名单候选换实时 CLOB 盘口价 (gamma 分钟级陈旧 — 实测让重挂同价加入 4220 股
        // 竞争墙后排队而非压前一档)。NO 书镜像: yes_ask = 1 − 最优竞争 NO bid, yes_bid = 1 − 最优
        // NO ask。必须剔除我们自己的挂单, 否则自己是最优买一时会被当竞争 → 每轮自我压价。
        if (wl_size != 0) {
            std::map<std::string, const tail::OpenOrder*> ours;
            for (const auto& o : open) ours[o.no_token] = &o;
            for (auto& [tok, c] : cands) {
                const auto book = client.get_order_book(tok);
                if (book.bids.empty() && book.asks.empty()) continue;  // 拉书失败 → 留 gamma 价
                const tail::OpenOrder* mine = nullptr;
                if (auto it = ours.find(tok); it != ours.end()) mine = it->second;
                double best_no_bid = 0.0, best_no_ask = 1.0;
                for (const auto& l : book.asks) best_no_ask = std::min(best_no_ask, l.price);
                for (const auto& l : book.bids) {
                    double sz = l.size;
                    if (mine != nullptr && std::abs(l.price - mine->no_price) < 5e-4) sz -= mine->size;
                    if (sz > 1e-6 && l.price > best_no_bid) best_no_bid = l.price;
                }
                if (best_no_bid > 0.0) c.yes_ask = 1.0 - best_no_bid;
                else if (mine != nullptr) c.yes_ask = 1.0 - mine->no_price;  // 全场只有我们 → 视稳
                if (best_no_ask < 1.0) c.yes_bid = 1.0 - best_no_ask;
            }
        }

        // ---- 3) 决策 (纯函数) -> 执行 ----
        // held 按 token 归属拆分: 触碰腿从 token_touch 子集聚合; core = 全量 − 触碰。两腿预算真正隔离
        // (触碰成交绝不占用 core 额度, 反之亦然)。token_touch 是本策略下过的触碰 token 注册表 (持久)。
        std::map<std::string, double> held_coin_touch;
        double held_total_touch = 0.0;
        for (const auto& [tok, amt] : st.token_held) {
            const auto tt = st.token_touch.find(tok);
            if (tt == st.token_touch.end() || !tt->second) continue;
            held_total_touch += amt;
            if (const auto ci = st.token_coin.find(tok); ci != st.token_coin.end())
                held_coin_touch[ci->second] += amt;
        }
        std::map<std::string, double> held_coin_core = st.coin;
        for (const auto& [coin, amt] : held_coin_touch)
            held_coin_core[coin] = std::max(0.0, held_coin_core[coin] - amt);   // 从 core 扣除触碰部分 (夹 ≥0)
        const double held_total_core = std::max(0.0, st.total - held_total_touch);
        const auto actions = tail::plan(cands, open, held_coin_core, st.token_held, held_total_core, cfg,
                                        held_coin_touch, held_total_touch);
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
            const std::string place_status = r.value("status", "");
            const int place_http = r.value("http", 0);
            // 传输层 (http==0: 超时/断连) 与服务端 (5xx) 瞬时故障 != 业务拒单: 订单未必真被拒,
            // 更不代表定价/授权/余额系统性坏。2026-07-06 一次 24s 网络抖动连拒 3 单 -> 误触 halt
            // -> bot 干净退出 8h 无人管 (挂单继续被吃)。故瞬时故障只退避, 绝不计入 halt。
            const bool transient = tail::is_transient_place_error(place_http);
            if (place_status == "PLACED") {
                consecutive_rejects = 0;
                consecutive_neterr = 0;
                // fill 归因注册 (只登记真下过单的 token — 状态文件保持米粒大)
                if (auto ic = cands.find(a.no_token); ic != cands.end()) {
                    st.token_coin[a.no_token] = ic->second.coin;
                    st.token_note[a.no_token] = ic->second.slug;
                    if (ic->second.is_touch) st.token_touch[a.no_token] = true;  // held 拆分用: 触碰腿标记
                    save_held(st, lf);
                }
            } else if (transient) {
                jlog(lf, {{"ev", "neterr"}, {"note", a.note}, {"http", place_http}});
                if (++consecutive_neterr >= 8) {  // 持续不可用 -> 本轮停手但保活到下一轮自愈
                    jlog(lf, {{"ev", "pause"}, {"why", "sustained transport errors — backing off this scan"}});
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(800));
                continue;  // 不计 executed, 不走 400ms 常规间隔
            } else {  // 真实 4xx 业务拒单 (收到响应 -> 连通性正常); 3 连拒 halt 保全现场等人查
                consecutive_neterr = 0;
                if (++consecutive_rejects >= 3) {
                    jlog(lf, {{"ev", "halt"}, {"why", "3 consecutive business rejects (http 4xx) — investigate"}});
                    g_run.store(false);
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            ++executed;
        }

        // ---- 5) 心跳/权益地板 ----
        // 每轮心跳 (事件驱动日志与死机不可区分 — 观测性要求每轮走表); 每 ~1h 附带余额。
        json hb = {{"ev", "scan"}, {"candidates", cands.size()}, {"open", open.size()},
                   {"held_usd", st.total}, {"planned", actions.size()}, {"executed", executed}};
        if (!st.coin.empty()) hb["held_coin"] = st.coin;
        if (wl_size != 0) hb["whitelist"] = wl_size;  // candidates 已是名单过滤后的数
        if (tick_count % 12 == 0 && armed)
            if (auto b = sub->usdc_balance()) hb["usdc"] = *b;
        jlog(lf, hb);
        } catch (const std::exception& e) {
            // 瞬时 API 故障 (CLOB 5xx/网络) 不炸进程 — 记日志, 睡到下一轮重试。实测 2026-07-05:
            // 一次 get_order_book 抛 ApiError → SIGABRT → systemd 重启 (状态无损, 但不该死)。
            jlog(lf, {{"ev", "error"}, {"what", std::string("scan_exception: ") + e.what()}});
        }
        ++tick_count;
        for (int i = 0; i < scan_s && g_run.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (std::ifstream("STOP_TAIL_VENDOR").good()) break;  // 1s 级急停响应
        }
    }
    jlog(lf, {{"ev", "exit"}, {"note", "resting GTC orders remain by design; restart resumes via reconcile"}});
    return 0;
}
