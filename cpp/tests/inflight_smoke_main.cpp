// tests/inflight_smoke_main.cpp — 锁死 InFlightLedger (滞后感知执行记账) 的对账逻辑。
//
// 核心: 链上结算延迟 ~25s 内, "相信的持仓 = 链上 + 在途", 链上追上后退役在途 (不双计), TTL 兜底。
// 验证净值假急停场景 (买入后链上低估 → 在途净成本补回真实净值)。
#include <cmath>
#include <cstdio>

#include "pmm/inflight.hpp"

using pmm::InFlightLedger;

static int g_fail = 0;
static void check(bool ok, const char* msg) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", msg);
    if (!ok) ++g_fail;
}
static bool near(double a, double b) { return std::abs(a - b) < 1e-6; }

int main() {
    // 1. 基本: 买入 → 在途净份额 + 净成本。
    {
        InFlightLedger l;
        l.on_fill("A", 300.0, 0.18, 0.0);  // 买 300 A @0.18
        check(near(l.believed_delta("A"), 300.0), "buy: believed_delta = +300");
        check(near(l.net_cost(), 54.0), "buy: net_cost = +$54");
        check(l.believed_positions({{"A", 0.0}})["A"] == 300.0, "buy: believed = chain(0)+inflight(300)");
    }

    // 2. 净值假急停修复: 买了 $54 但链上还显示 0 → 净值 = USDC + 链上(0) + 在途净成本($54) = 真实值。
    {
        InFlightLedger l;
        const double usdc = 859.0;  // 买 $54 后 USDC 立刻降 (914→859)
        l.on_fill("X", 300.0, 0.18, 0.0);
        const double chain_val = 0.0;  // 链上持仓市值滞后 = 0
        const double equity = usdc + chain_val + l.net_cost();
        check(near(equity, 913.0), "equity: USDC+chain(lag 0)+inflight = $913 (no false drop)");
    }

    // 3. 对账退役: 链上追上买入 → 在途退役, 不双计。
    {
        InFlightLedger l;
        l.on_fill("A", 100.0, 0.5, 0.0);
        l.reconcile({{"A", 100.0}}, 10.0);  // 链上现在反映了这 100
        check(near(l.believed_delta("A"), 0.0), "reconcile full: inflight retired");
        check(l.believed_positions({{"A", 100.0}})["A"] == 100.0, "reconcile full: believed=100 not 200 (no double)");
    }

    // 4. 部分追上: 链上只反映了 60 → 在途余 40, 仍相信总 100。
    {
        InFlightLedger l;
        l.on_fill("A", 100.0, 0.5, 0.0);
        l.reconcile({{"A", 60.0}}, 10.0);
        check(near(l.believed_delta("A"), 40.0), "reconcile partial: 40 still in-flight");
        check(l.believed_positions({{"A", 60.0}})["A"] == 100.0, "reconcile partial: believed still 100");
    }

    // 5. TTL 老化: 链上始终不反映 (失败单/漏匹配) → 超 45s 强制退役, 防泄漏。
    {
        InFlightLedger l;
        l.on_fill("A", 100.0, 0.5, 0.0);
        l.reconcile({}, 46.0);  // 46s 后, 链上仍空
        check(l.size() == 0 && near(l.believed_delta("A"), 0.0), "TTL: aged out after 45s");
    }

    // 6. 买后卖净额 + 卖出对账退役。
    {
        InFlightLedger l;
        l.reconcile({{"A", 100.0}}, 0.0);  // 先确立 prev_chain (持有 100)
        l.on_fill("A", -30.0, 0.6, 1.0);   // 卖 30 (taker)
        check(near(l.believed_delta("A"), -30.0), "sell: believed_delta = -30 (in-flight)");
        check(l.believed_positions({{"A", 100.0}})["A"] == 70.0, "sell: believed = chain(100)-inflight(30)=70");
        l.reconcile({{"A", 70.0}}, 5.0);  // 链上追上卖出
        check(near(l.believed_delta("A"), 0.0), "sell: retired after chain reflects");
    }

    // 7. 外部/无在途时链上移动 → 不乱退役, believed = 链上。
    {
        InFlightLedger l;
        l.reconcile({{"A", 50.0}}, 0.0);
        check(l.believed_positions({{"A", 50.0}})["A"] == 50.0, "no-inflight: believed follows chain");
    }

    std::printf(g_fail == 0 ? "ALL CHECKS PASSED\n" : "FAILED (%d)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
