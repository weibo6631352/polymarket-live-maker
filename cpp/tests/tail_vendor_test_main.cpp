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
    // 4) 接受: reach 触碰上行尾 (2026-07-09 触碰腿 — 方向靠白名单 + curator K>spot 把关, 此处放行)
    auto cr = parse_candidate(row("will-bitcoin-reach-150k-by-december-31-2026",
                                  "Will Bitcoin reach $150k?", 0.03, 0.04,
                                  "2026-12-31T23:59:00Z"), kNow);
    assert(cr && cr->coin == "btc");
    // 4b) 拒绝: 下行触碰词 (dip/fall/新低) — 纵深防御, 不靠单一 curator 把关
    assert(!parse_candidate(row("will-bitcoin-dip-to-50k", "Will Bitcoin dip to $50k?", 0.03, 0.04,
                                "2026-12-31T23:59:00Z"), kNow));
    assert(!parse_candidate(row("will-bitcoin-fall-to-50k", "Will Bitcoin fall to $50k?", 0.03, 0.04,
                                "2026-12-31T23:59:00Z"), kNow));
    assert(!parse_candidate(row("will-bitcoin-reach-new-all-time-low",
                                "Will Bitcoin reach a new all-time low of $30k?", 0.03, 0.04,
                                "2026-12-31T23:59:00Z"), kNow));
    // 4c) 拒绝: "hit" 已不再收 ("hit $50k" 下行 / "hit all-time high" 无锚 都不是我们的盘)
    assert(!parse_candidate(row("will-bitcoin-hit-50k", "Will Bitcoin hit $50k?", 0.03, 0.04,
                                "2026-12-31T23:59:00Z"), kNow));
    assert(!parse_candidate(row("will-bitcoin-hit-ath", "Will Bitcoin hit a new all-time high?",
                                0.03, 0.04, "2026-12-31T23:59:00Z"), kNow));
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
    auto q = decide(*c, cfg, 0.0, 0.0, 0.0);
    assert(q && std::abs(q->no_price - 0.971) < 1e-9 && q->size == 20 && q->no_token == "222");
    // 9) floor: ask=0.021 -> undercut 0.020 = floor, 不低于
    auto cf = *c; cf.yes_ask = 0.021; cf.yes_bid = 0.010;
    auto qf = decide(cf, cfg, 0.0, 0.0, 0.0);
    assert(qf && std::abs((1.0 - qf->no_price) - 0.02) < 1e-9);
    // 10) 带外拒绝: ask 太高
    auto ch = *c; ch.yes_ask = 0.12; ch.yes_bid = 0.10;
    assert(!decide(ch, cfg, 0.0, 0.0, 0.0));
    // 11) 不越过买一: bid==undercut 价 -> 拒绝
    auto cx = *c; cx.yes_ask = 0.03; cx.yes_bid = 0.029;
    assert(!decide(cx, cfg, 0.0, 0.0, 0.0));
    // 12) 资金上限: per_coin 已满 -> 拒绝
    assert(!decide(*c, cfg, 0.0, 59.0, 0.0));
    assert(!decide(*c, cfg, 0.0, 0.0, 59.0));
    // 12b) 非对称无锚上限 (per_coin_alt_usd): 同一 deployed=100, btc 过 (cap 200) / sol,xrp 拒 (cap 75);
    //      alt=0 时回落 per_coin_usd (向后兼容)。
    {
        Config ca = cfg; ca.per_coin_usd = 200; ca.per_coin_alt_usd = 75; ca.total_usd = 500;
        auto cbtc = *c; cbtc.coin = "btc";
        auto csol = *c; csol.coin = "sol";
        auto cxrp = *c; cxrp.coin = "xrp";
        assert(decide(cbtc, ca, 0.0, 100.0, 0.0));   // btc 100 < 200 -> 过
        assert(!decide(csol, ca, 0.0, 100.0, 0.0));  // sol 100 > 75 -> 拒 (无锚段收紧)
        assert(!decide(cxrp, ca, 0.0, 100.0, 0.0));  // xrp 同
        assert(decide(csol, ca, 0.0, 40.0, 0.0));    // sol 40 (+~20 名义) < 75 -> 过
        Config cb = cfg; cb.per_coin_usd = 200; cb.per_coin_alt_usd = 0; cb.total_usd = 500;
        auto csol2 = *c; csol2.coin = "sol";
        assert(decide(csol2, cb, 0.0, 100.0, 0.0));  // alt=0 回落 200 -> sol 100 过 (向后兼容)
    }
    // 13) 期限窗: 太远
    auto cd = *c; cd.days_left = 30;
    assert(!decide(cd, cfg, 0.0, 0.0, 0.0));
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
    auto acts = plan(cands, {}, {}, {}, 0.0, cfg);
    int places = 0;
    for (const auto& a : acts) places += (a.kind == Action::Kind::kPlace);
    assert(places == 3);
    // 16) held 计入总上限: 已持 $45 -> 只能再报 0 个 (45+19.4*1 <60 -> 实际 1 个? 45+19.4=64.4>60 -> 0)
    acts = plan(cands, {}, {}, {}, 45.0, cfg);
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
    acts = plan(cb, {}, {}, {}, 0.0, cfg2);
    places = 0;
    for (const auto& a : acts) places += (a.kind == Action::Kind::kPlace);
    assert(places == 2);  // 2×19.42=38.8 <= 40; 第 3 个越限
    // 18) 撤单释放预算: 在场 1 个 left_window 单 (不在 cands) + 预算刚好 -> 撤它并报新
    std::vector<OpenOrder> open1 = {{"gone_tok", 0.97, 20, "btc", "old"}};
    Config cfg3 = cfg; cfg3.total_usd = 25;  // 只够一单
    std::map<std::string, Candidate> c1;
    { Candidate cc = *c; cc.no_token = "n1"; cc.coin = "eth"; cc.yes_bid = 0.02; cc.yes_ask = 0.03;
      cc.days_left = 2; cc.slug = "n1"; c1["n1"] = cc; }
    acts = plan(c1, open1, {}, {}, 0.0, cfg3);
    assert(acts.size() == 2 && acts[0].kind == Action::Kind::kCancel &&
           acts[0].why == "left_window" && acts[1].kind == Action::Kind::kPlace);
    // 19) 留场单占预算: 在场单是活跃候选且价仍最优 -> 不撤不重报, 预算被占
    std::vector<OpenOrder> open2 = {{"n1", 0.971, 20, "eth", "n1"}};  // sell_yes 0.029, ask 0.03 -> 仍最优
    acts = plan(c1, open2, {}, {}, 0.0, cfg3);
    assert(acts.empty());
    // 20) 被压价 -> 撤 (ask 已低于我们的隐含卖价 - tick, 且重挂价能改进)
    std::vector<OpenOrder> open3 = {{"n1", 0.960, 20, "eth", "n1"}};  // 我们卖 0.040, ask 0.03 更优
    acts = plan(c1, open3, {}, {}, 0.0, cfg3);
    bool has_outbid_cancel = false;
    for (const auto& a : acts)
        if (a.kind == Action::Kind::kCancel && a.why == "outbid") has_outbid_cancel = true;
    assert(has_outbid_cancel);
    // 20b) 竞争墙严格压在上方且地板卡死 (重挂无法改进) -> 撤单轮换 (排队死资本; 2026-07-04
    //      实测尾部竞争方是 4-5k 股挂墙 bot, 排它后面 = 永不成交)。decide 的 skip 规则保证
    //      撤后不会在同市场原价重挂 (无 churn)。
    std::map<std::string, Candidate> c2m;
    { Candidate cc = *c; cc.no_token = "n2"; cc.coin = "eth"; cc.yes_bid = 0.010; cc.yes_ask = 0.016;
      cc.days_left = 2; cc.slug = "n2"; c2m["n2"] = cc; }
    std::vector<OpenOrder> open4 = {{"n2", 0.980, 20, "eth", "n2"}};  // 我们卖 0.020 = floor, 墙卖 0.016
    acts = plan(c2m, open4, {}, {}, 0.0, cfg3);
    bool rotated = false;
    for (const auto& a : acts) {
        assert(a.kind != Action::Kind::kPlace || a.no_token != "n2");  // skip 规则: 不原地重挂
        if (a.kind == Action::Kind::kCancel && a.why == "wall_over_floor") rotated = true;
    }
    assert(rotated);
    // 20c) 1 tick 以内被压 -> 滞回保留 (不值得为 1 tick 打 undercut 战)
    std::map<std::string, Candidate> c3m;
    { Candidate cc = *c; cc.no_token = "n3"; cc.coin = "eth"; cc.yes_bid = 0.010; cc.yes_ask = 0.025;
      cc.days_left = 2; cc.slug = "n3"; c3m["n3"] = cc; }
    std::vector<OpenOrder> open5 = {{"n3", 0.974, 20, "eth", "n3"}};  // 我们卖 0.026, 墙 0.025 (1 tick)
    acts = plan(c3m, open5, {}, {}, 0.0, cfg3);
    for (const auto& a : acts) assert(a.kind != Action::Kind::kCancel);
    // 20d) decide skip: 竞争卖压已在地板下 (ask 0.019 < floor 0.02) -> 排不到前面, 不下单
    auto cw = *c; cw.yes_ask = 0.019; cw.yes_bid = 0.005;
    assert(!decide(cw, cfg, 0.0, 0.0, 0.0));
    // 20f) 首卖 clamp: ask 肥 (0.5) 时默认跳过; band_clamp 授权 -> 站到带顶 7c (NO 0.93)
    auto cfat = *c; cfat.yes_ask = 0.50; cfat.yes_bid = 0.01;
    assert(!decide(cfat, cfg, 0.0, 0.0, 0.0));
    cfat.band_clamp = true;
    auto qc = decide(cfat, cfg, 0.0, 0.0, 0.0);
    assert(qc && std::abs((1.0 - qc->no_price) - 0.07) < 1e-9);
    // 20g) clamp 但买盘已越过带顶 (bid 0.08 > 0.07) -> 拒绝 (不越 bid)
    cfat.yes_bid = 0.08;
    assert(!decide(cfat, cfg, 0.0, 0.0, 0.0));

    // 20h) clamp 盘的 pull: 垃圾书 (bid 0.01/ask 0.96) 不误杀; 买一真抬到 0.10+ 才撤
    auto cjunk = *c; cjunk.band_clamp = true; cjunk.yes_bid = 0.01; cjunk.yes_ask = 0.96;
    assert(!should_pull(cjunk, cfg));
    cjunk.yes_bid = 0.11;
    assert(should_pull(cjunk, cfg));

    // 20e) 单市场集中度 cap: 该市场已部署接近 per_market_usd -> 拒绝 (同一买家反复加注防吃穿)
    assert(!decide(*c, cfg, cfg.per_market_usd - 1.0, 0.0, 0.0));
    // plan 侧: held_token 里已计入的市场不再补挂
    std::map<std::string, Candidate> cmk;
    { Candidate cc = *c; cc.no_token = "m1"; cc.coin = "eth"; cc.yes_bid = 0.02; cc.yes_ask = 0.03;
      cc.days_left = 2; cc.slug = "m1"; cmk["m1"] = cc; }
    acts = plan(cmk, {}, {}, {{"m1", cfg.per_market_usd - 1.0}}, 0.0, cfg);
    for (const auto& a : acts) assert(a.kind != Action::Kind::kPlace);

    // ---- 触碰/reach 卫星腿: 独立预算 + per-row sizing (2026-07-09 (b) 集成) ----
    {
        auto mkt = [&](const char* tok, const char* coin, double coll) {
            Candidate cc = *c;
            cc.no_token = tok; cc.coin = coin; cc.slug = tok;
            cc.yes_bid = 0.02; cc.yes_ask = 0.04; cc.days_left = 2;   // 卖 0.039 -> NO 0.961
            cc.is_touch = true; cc.wl_collateral = coll;
            return cc;
        };
        Config tc = cfg;   // core caps 默认, 触碰预算先全 0 (未武装)
        // T1) 触碰未武装 (touch_total=0) -> 拒触碰单, 即便白名单给了 collateral
        assert(!decide(mkt("u1", "btc", 24.0), tc, 0.0, 0.0, 0.0));
        tc.touch_total_usd = 80; tc.touch_per_coin_usd = 48;
        tc.touch_per_order_usd = 24; tc.touch_per_market_usd = 24;
        // T2) size 由 per-row collateral 决定 (24/0.961=24 股), 而非 core 的 per_order_usd(20 -> 20 股)
        auto qt = decide(mkt("u2", "btc", 24.0), tc, 0.0, 0.0, 0.0);
        assert(qt && qt->size == std::floor(24.0 / qt->no_price) && qt->size > 20);
        // T3) per-order 硬顶: collateral=40 夹到 touch_per_order=24
        auto qcap = decide(mkt("u3", "btc", 40.0), tc, 0.0, 0.0, 0.0);
        assert(qcap && qcap->size == std::floor(24.0 / qcap->no_price));
        // T4) 触碰用 touch_total(80) 而非 core total(60): touch 已部署 50 -> 下得出; 60 -> 越限
        assert(decide(mkt("u4", "btc", 24.0), tc, 0.0, 0.0, 50.0));   // 50+23<80 (core 会在 >60 拒)
        assert(!decide(mkt("u4b", "btc", 24.0), tc, 0.0, 0.0, 60.0));  // 60+23=83>80 拒
        // T5) 预算隔离: 巨额 core held (sol $999) 不吃触碰预算 -> 两个触碰 sol 单照下
        std::map<std::string, Candidate> tcands;
        tcands["tt1"] = mkt("tt1", "sol", 24.0);
        tcands["tt2"] = mkt("tt2", "sol", 24.0);
        auto ta = plan(tcands, {}, {{"sol", 999.0}}, {}, 999.0, tc);   // core held/total 巨大
        int tp = 0; for (const auto& a : ta) tp += (a.kind == Action::Kind::kPlace);
        assert(tp == 2);
        // T6) 触碰 per_coin(48) 上限: 同币第 3 个越限
        tcands["tt3"] = mkt("tt3", "sol", 24.0);
        ta = plan(tcands, {}, {}, {}, 0.0, tc);
        tp = 0; for (const auto& a : ta) tp += (a.kind == Action::Kind::kPlace);
        assert(tp == 2);
        // T7) 混合轮: core 候选走 core 预算, 触碰候选走触碰预算, 互不影响
        auto core_c = *c; core_c.no_token = "cc1"; core_c.coin = "btc"; core_c.slug = "cc1";
        core_c.yes_bid = 0.02; core_c.yes_ask = 0.04; core_c.days_left = 2;  // is_touch=false
        std::map<std::string, Candidate> mix;
        mix["cc1"] = core_c; mix["tt1"] = mkt("tt1", "sol", 24.0);
        auto ma = plan(mix, {}, {}, {}, 0.0, tc);
        int core_place = 0, touch_place = 0;
        for (const auto& a : ma)
            if (a.kind == Action::Kind::kPlace) (a.note == std::string("cc1") ? core_place : touch_place)++;
        assert(core_place == 1 && touch_place == 1);
        // T8) core 优先: 只剩 1 个 max_orders 名额, core+touch 各一候选 -> core 先占, 触碰不挤占
        Config tc1 = tc; tc1.max_orders = 1;
        auto ma1 = plan(mix, {}, {}, {}, 0.0, tc1);
        int core_p1 = 0, touch_p1 = 0;
        for (const auto& a : ma1)
            if (a.kind == Action::Kind::kPlace) (a.note == std::string("cc1") ? core_p1 : touch_p1)++;
        assert(core_p1 == 1 && touch_p1 == 0);
        // T9) 反向隔离 (bug #1): 巨额触碰 held 传入不挡 core 下单 (core 只看 core 预算)
        std::map<std::string, Candidate> ccore; ccore["cc1"] = core_c;
        auto ma9 = plan(ccore, {}, {}, {}, 0.0, tc, {{"btc", 999.0}}, 999.0);  // touch held 巨大
        int core_p9 = 0; for (const auto& a : ma9) core_p9 += (a.kind == Action::Kind::kPlace);
        assert(core_p9 == 1);
    }

    // ---- sheet 模式: 解析 + 校验 ----
    // 21) 正常解析
    json sj = json::array({{{"token_id", "123"}, {"price", 0.97}, {"size", 10.0}, {"note", "a"}},
                           {{"token_id", "456"}, {"price", 0.95}, {"size", 20.0}}});
    auto sh = parse_sheet(sj);
    assert(sh && sh->size() == 2 && (*sh)[0].no_price == 0.97 && (*sh)[1].note.empty());
    // 22) 类型错 -> 整体失败 (不静默跳行)
    json sbad = json::array({{{"token_id", "123"}, {"price", "0.97"}, {"size", 10.0}}});
    assert(!parse_sheet(sbad));
    // 23) 校验通过
    assert(!validate_sheet(*sh, cfg).has_value());
    // 24) 价格带 veto (买 YES 价被拒 — 防止把 YES 单误写成 NO 单)
    json slow = json::array({{{"token_id", "123"}, {"price", 0.05}, {"size", 10.0}}});
    assert(validate_sheet(*parse_sheet(slow), cfg).has_value());
    // 25) 总抵押 veto
    json sbig = json::array({{{"token_id", "123"}, {"price", 0.97}, {"size", 40.0}},
                             {{"token_id", "456"}, {"price", 0.97}, {"size", 40.0}}});
    assert(validate_sheet(*parse_sheet(sbig), cfg).has_value());  // 77.6 > 60
    // 26) 重复 token veto
    json sdup = json::array({{{"token_id", "123"}, {"price", 0.97}, {"size", 10.0}},
                             {{"token_id", "123"}, {"price", 0.96}, {"size", 10.0}}});
    assert(validate_sheet(*parse_sheet(sdup), cfg).has_value());

    // 27) 下单 response 分类: 传输层/5xx = 瞬时 (退避, 不 halt); 4xx/2xx-unmatched = 业务拒单 (计 halt)
    assert(is_transient_place_error(0));      // 超时/断连 (curl 无 http 响应) -> 2026-07-06 事故根因
    assert(is_transient_place_error(500));    // 服务端错误
    assert(is_transient_place_error(503));    // 服务不可用
    assert(!is_transient_place_error(400));   // 坏请求 (坏定价/参数) -> 真拒单
    assert(!is_transient_place_error(401));   // 授权失败 -> 真拒单
    assert(!is_transient_place_error(429));   // 限流 -> 业务信号, 该退但计入拒单谱系
    assert(!is_transient_place_error(200));   // 2xx-but-unmatched 的业务拒单

    std::printf("tail_vendor: all tests PASS (incl. touch-leg sizing/budget-isolation)\n");
    return 0;
}
