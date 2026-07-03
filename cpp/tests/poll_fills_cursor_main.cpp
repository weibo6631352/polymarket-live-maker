// tests/poll_fills_cursor_main.cpp — 锁死 poll_fills 游标逻辑 (两次真钱孤立事故的根因)。
//
// 旧 bug: break 比的是循环里"刚被推进"的 last_id, 而非进入时的旧游标 → 永不 break → 返回全部 trades,
// 撑爆 runner 去重 → 旧成交反复当新 → 库存账本错乱 → 仓位孤立。修复后只返回比旧游标更新的成交。
#include <cstdio>
#include <optional>
#include <set>
#include <string>

#include "pmm/clob_submitter.hpp"

using namespace pmm::clob;
using nlohmann::json;

static int g_fail = 0;
static void check(bool ok, const char* msg) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", msg);
    if (!ok) ++g_fail;
}
static json tr(const char* id, const char* tok, const char* side) {
    return {{"id", id}, {"asset_id", tok}, {"side", side}, {"size", 10.0}, {"price", 0.5}};
}

int main() {
    std::set<std::string> own;

    // 1. 启动 (空游标) + [t3,t2,t1] → 全部 3 笔, 游标推到 t3。
    json d3 = json::array({tr("t3", "A", "BUY"), tr("t2", "A", "BUY"), tr("t1", "A", "BUY")});
    std::optional<std::string> cur;
    auto r1 = ClobSubmitter::extract_new_fills(d3, cur, own, false);
    check(r1.size() == 3 && cur && *cur == "t3", "startup: empty cursor returns all, advances to newest");

    // 2. 无新成交: 游标=t3 + [t3,t2,t1] → 0 笔。
    auto r2 = ClobSubmitter::extract_new_fills(d3, cur, own, false);
    check(r2.size() == 0, "no-new: cursor at newest returns 0");

    // 3. 关键回归: 游标=t3 + [t5,t4,t3,t2,t1] → 只 2 笔 (t5,t4), 游标=t5。
    //    旧 bug 在这里返回 5 笔 (永不 break) —— 正是撑爆去重的根因。
    json d5 = json::array({tr("t5", "A", "BUY"), tr("t4", "B", "SELL"), tr("t3", "A", "BUY"),
                           tr("t2", "A", "BUY"), tr("t1", "A", "BUY")});
    auto r3 = ClobSubmitter::extract_new_fills(d5, cur, own, false);
    check(r3.size() == 2 && cur && *cur == "t5", "two-new: returns only newer-than-cursor (old bug returned all 5)");
    check(r3.size() >= 1 && r3[0].value("id", std::string{}) == "t5" &&
              r3[0].value("token_id", std::string{}) == "A" && r3[0].value("side", std::string{}) == "BUY",
          "fill fields parsed (id/token/side)");

    // 4. 排除自己的 flatten(taker) 腿。
    std::set<std::string> own2 = {"ord9"};
    json d = json::array(
        {json{{"id", "t6"}, {"asset_id", "A"}, {"side", "BUY"}, {"size", 10.0}, {"taker_order_id", "ord9"}},
         tr("t5b", "A", "BUY")});
    std::optional<std::string> cur2 = std::string("t5b");
    auto r4 = ClobSubmitter::extract_new_fills(d, cur2, own2, false);
    check(r4.size() == 0, "own-taker fill excluded");

    // 5. invert_side: BUY<->SELL 翻转 (proxy 语义)。
    json d6 = json::array({tr("t7", "A", "BUY")});
    std::optional<std::string> cur3 = std::string("t6x");
    auto r5 = ClobSubmitter::extract_new_fills(d6, cur3, own, true);
    check(r5.size() == 1 && r5[0].value("side", std::string{}) == "SELL", "invert_side flips BUY->SELL");

    // 6. maker 口径 (2026-07-03 实录 trade 1a52336e, 字段为 string): 顶层是 taker 视角 (对侧 YES token,
    //    taker 总量 20, 混合价 0.05) — 我们的真实腿在 maker_orders[]: BUY NO 15@0.953。老口径把 20 股
    //    记到对侧 token 上 (幻影库存, 实测余额差=15×0.953 证伪); 新口径必须只取我们的腿, 且不套 invert。
    json real = json::array({json{
        {"id", "1a52336e"},
        {"asset_id", "YES_TOK"},
        {"side", "BUY"},
        {"size", "20"},
        {"price", "0.05"},
        {"trader_side", "MAKER"},
        {"taker_order_id", "0x9fe4"},
        {"maker_orders", json::array({
            json{{"asset_id", "NO_TOK"}, {"maker_address", "0x78dEAbCd"}, {"matched_amount", "15"},
                 {"order_id", "0xadec"}, {"outcome", "No"}, {"price", "0.953"}, {"side", "BUY"}},
            json{{"asset_id", "NO_TOK"}, {"maker_address", "0x6Fd0"}, {"matched_amount", "5"},
                 {"order_id", "0x87b9"}, {"outcome", "No"}, {"price", "0.941"}, {"side", "BUY"}},
        })}}});
    std::optional<std::string> cur4;
    auto r6 = ClobSubmitter::extract_new_fills(real, cur4, own, /*invert=*/true, "0x78deabcd");
    check(r6.size() == 1, "maker view: exactly our one leg (not taker total, not the other maker)");
    check(r6.size() == 1 && r6[0].value("token_id", std::string{}) == "NO_TOK" &&
              r6[0].value("size", 0.0) == 15.0 && r6[0].value("price", 0.0) == 0.953 &&
              r6[0].value("side", std::string{}) == "BUY" &&
              r6[0].value("order_id", std::string{}) == "0xadec",
          "maker view: our leg's asset/size/price/side/order_id (invert NOT applied to explicit legs)");
    check(cur4 && *cur4 == "1a52336e", "maker view: cursor still advances per-trade");

    // 7. maker 口径下纯 taker 行 (maker_orders 全是别人) → 0 笔。
    std::optional<std::string> cur5;
    auto r7 = ClobSubmitter::extract_new_fills(real, cur5, own, false, "0xnobody");
    check(r7.size() == 0, "maker view: trade with no leg of ours emits nothing");

    // 8. maker_lc 给定但行无 maker_orders (旧 schema) → 顶层兜底口径仍工作。
    json legacy = json::array({tr("t8", "A", "BUY")});
    std::optional<std::string> cur6;
    auto r8 = ClobSubmitter::extract_new_fills(legacy, cur6, own, false, "0x78deabcd");
    check(r8.size() == 1 && r8[0].value("token_id", std::string{}) == "A" &&
              r8[0].value("size", 0.0) == 10.0,
          "maker view: rows without maker_orders fall back to top-level fields");

    std::printf(g_fail ? "POLL_FILLS CURSOR: FAIL=%d\n" : "POLL_FILLS CURSOR: ALL PASS\n", g_fail);
    return g_fail;
}
