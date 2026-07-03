// tests/tail_vendor_test_main.cpp — 决策核纯函数单测 (离线, 无网络)。退出码 0 = 全过。
#undef NDEBUG  // RelWithDebInfo 定义 NDEBUG 会把 assert 编译掉 -> 空测; 单测必须始终真跑
#include <cassert>
#include <cstdio>

#include <nlohmann/json.hpp>

#include "pmm/tail_vendor.hpp"

using nlohmann::json;
using namespace pmm::tail;

namespace {
json row(const char* slug, const char* q, double bb, double ba, const char* end) {
    return {{"slug", slug}, {"question", q}, {"conditionId", "0xabc"},
            {"outcomes", json::array({"Yes", "No"})},
            {"clobTokenIds", json::array({"111", "222"})},
            {"bestBid", bb}, {"bestAsk", ba}, {"endDate", end}};
}
constexpr double kNow = 1783100000.0;  // 2026-07-03T17:33Z 附近
}  // namespace

int main() {
    // 1) 接受: strike daily 上行尾
    auto c = parse_candidate(row("bitcoin-above-70k-on-july-6-2026",
                                 "Will Bitcoin be above $70k on July 6?", 0.02, 0.03,
                                 "2026-07-06T16:00:00Z"), kNow);
    assert(c && c->coin == "btc" && c->no_token == "222");
    // 2) 接受: negrisk bucket "greater than"
    auto c2 = parse_candidate(row("what-price-will-solana-be-on-july-8",
                                  "Will the price of Solana be greater than $220 on July 8?",
                                  0.015, 0.025, "2026-07-08T16:00:00Z"), kNow);
    assert(c2 && c2->coin == "sol");
    // 3) 拒绝: 下行方向
    assert(!parse_candidate(row("bitcoin-below-50k-on-july-6-2026",
                                "Will Bitcoin be below $50k?", 0.02, 0.03,
                                "2026-07-06T16:00:00Z"), kNow));
    // 4) 拒绝: touch 家族 (reach/hit/dip/ath)
    assert(!parse_candidate(row("will-bitcoin-reach-150k-by-december-31-2026",
                                "Will Bitcoin reach $150k?", 0.03, 0.04,
                                "2026-12-31T23:59:00Z"), kNow));
    // 5) 拒绝: 微市场
    assert(!parse_candidate(row("bitcoin-up-or-down-july-6-3pm-et",
                                "Bitcoin Up or Down?", 0.4, 0.6, "2026-07-06T19:05:00Z"), kNow));
    // 6) 拒绝: outcomes 不是 Yes/No
    json bad = row("bitcoin-above-70k-on-july-6-2026", "above?", 0.02, 0.03, "2026-07-06T16:00:00Z");
    bad["outcomes"] = json::array({"Up", "Down"});
    assert(!parse_candidate(bad, kNow));
    // 7) 拒绝: DOGE (校准不足)
    assert(!parse_candidate(row("dogecoin-above-1-on-july-6-2026", "Doge above $1?", 0.02, 0.03,
                                "2026-07-06T16:00:00Z"), kNow));

    Config cfg;  // 默认: band [0.02,0.07], per_order 20, per_coin 60, total 60
    // 8) 报价: 压 ask 一 tick -> sell_yes=0.029, no=0.971, size=floor(20/0.971)=20
    auto q = decide(*c, cfg, 0.0, 0.0);
    assert(q && std::abs(q->no_price - 0.971) < 1e-9 && q->size == 20 && q->no_token == "222");
    // 9) floor: ask=0.021 -> undercut 0.020 = floor, 不低于
    auto cf = *c; cf.yes_ask = 0.021; cf.yes_bid = 0.010;
    auto qf = decide(cf, cfg, 0.0, 0.0);
    assert(qf && std::abs((1.0 - qf->no_price) - 0.02) < 1e-9);
    // 10) 带外拒绝: ask 太高
    auto ch = *c; ch.yes_ask = 0.12; ch.yes_bid = 0.10;
    assert(!decide(ch, cfg, 0.0, 0.0));
    // 11) 不越过买一: bid==undercut 价 -> 拒绝
    auto cx = *c; cx.yes_ask = 0.03; cx.yes_bid = 0.029;
    assert(!decide(cx, cfg, 0.0, 0.0));
    // 12) 资金上限: per_coin 已满 -> 拒绝
    assert(!decide(*c, cfg, 59.0, 0.0));
    assert(!decide(*c, cfg, 0.0, 59.0));
    // 13) 期限窗: 太远
    auto cd = *c; cd.days_left = 30;
    assert(!decide(cd, cfg, 0.0, 0.0));
    // 14) should_pull: 逼近障碍
    auto cp = *c; cp.yes_bid = 0.09; cp.yes_ask = 0.12;
    assert(should_pull(cp, cfg));
    assert(!should_pull(*c, cfg));

    // ---- plan(): 一轮决策的上限/撤补语义 ----
    std::map<std::string, Candidate> cands;
    auto mk = [&](const char* tok, const char* coin, double bid, double ask, double days) {
        Candidate cc = *c;
        cc.no_token = tok; cc.coin = coin; cc.yes_bid = bid; cc.yes_ask = ask; cc.days_left = days;
        cc.slug = tok;
        cands[tok] = cc;
    };
    // 15) 总上限在一轮内被严格执行: 4 个候选 × ~$19.4, total=60 -> 只能报 3 个
    mk("t1", "btc", 0.02, 0.03, 2); mk("t2", "eth", 0.02, 0.03, 2);
    mk("t3", "sol", 0.02, 0.03, 2); mk("t4", "xrp", 0.02, 0.03, 2);
    auto acts = plan(cands, {}, {}, 0.0, cfg);
    int places = 0;
    for (const auto& a : acts) places += (a.kind == Action::Kind::kPlace);
    assert(places == 3);
    // 16) held 计入总上限: 已持 $45 -> 只能再报 0 个 (45+19.4*1 <60 -> 实际 1 个? 45+19.4=64.4>60 -> 0)
    acts = plan(cands, {}, {}, 45.0, cfg);
    places = 0;
    for (const auto& a : acts) places += (a.kind == Action::Kind::kPlace);
    assert(places == 0);
    // 17) 单币上限: 同币 4 个候选, per_coin=60 -> 3 个 (但 total=60 也是 3) -> 用 per_coin=40 验证
    Config cfg2 = cfg; cfg2.per_coin_usd = 40; cfg2.total_usd = 200;
    std::map<std::string, Candidate> cb;
    for (const char* t : {"b1", "b2", "b3", "b4"}) {
        Candidate cc = *c; cc.no_token = t; cc.coin = "btc"; cc.yes_bid = 0.02; cc.yes_ask = 0.03;
        cc.days_left = 2; cc.slug = t; cb[t] = cc;
    }
    acts = plan(cb, {}, {}, 0.0, cfg2);
    places = 0;
    for (const auto& a : acts) places += (a.kind == Action::Kind::kPlace);
    assert(places == 2);  // 2×19.42=38.8 <= 40; 第 3 个越限
    // 18) 撤单释放预算: 在场 1 个 left_window 单 (不在 cands) + 预算刚好 -> 撤它并报新
    std::vector<OpenOrder> open1 = {{"gone_tok", 0.97, 20, "btc", "old"}};
    Config cfg3 = cfg; cfg3.total_usd = 25;  // 只够一单
    std::map<std::string, Candidate> c1;
    { Candidate cc = *c; cc.no_token = "n1"; cc.coin = "eth"; cc.yes_bid = 0.02; cc.yes_ask = 0.03;
      cc.days_left = 2; cc.slug = "n1"; c1["n1"] = cc; }
    acts = plan(c1, open1, {}, 0.0, cfg3);
    assert(acts.size() == 2 && acts[0].kind == Action::Kind::kCancel &&
           acts[0].why == "left_window" && acts[1].kind == Action::Kind::kPlace);
    // 19) 留场单占预算: 在场单是活跃候选且价仍最优 -> 不撤不重报, 预算被占
    std::vector<OpenOrder> open2 = {{"n1", 0.971, 20, "eth", "n1"}};  // sell_yes 0.029, ask 0.03 -> 仍最优
    acts = plan(c1, open2, {}, 0.0, cfg3);
    assert(acts.empty());
    // 20) 被压价 -> 撤 (ask 已低于我们的隐含卖价 - tick)
    std::vector<OpenOrder> open3 = {{"n1", 0.960, 20, "eth", "n1"}};  // 我们卖 0.040, ask 0.03 更优
    acts = plan(c1, open3, {}, 0.0, cfg3);
    bool has_outbid_cancel = false;
    for (const auto& a : acts)
        if (a.kind == Action::Kind::kCancel && a.why == "outbid") has_outbid_cancel = true;
    assert(has_outbid_cancel);

    std::printf("tail_vendor: 20/20 PASS\n");
    return 0;
}
