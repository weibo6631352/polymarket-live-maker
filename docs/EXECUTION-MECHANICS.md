# 执行机制档案 — 每种交易机制的耗时与成功率(回测↔实盘校准层)

> 用户指令(2026-07-03):"一定要弄清楚各种交易机制所需要花费的时间和成功率。这是真钱模式是否和回测
> 一致的关键 —— 真钱经常遇到买不到、卖得慢、价格已亏损等问题。"
>
> 本文汇编本项目**所有已实测**的执行参数(含来源),标注**未知项**及其测量方案。
> 规则:任何策略上线前,其回测必须显式消费本表参数;最小真钱试验的首要目的是补齐本表,其次才是验证盈亏。

历次实盘亏损的根因几乎全部落在本表,而非"策略方向错":sniper 回测假设 100% 成交、实测 17%;
GTC 假死单成为孤儿仓;结算可见性滞后打崩平仓与风控;scalp 事故源于成交检测错误。

## 一、已实测(带来源)

| # | 机制 | 耗时 | 成功率 / 关键事实 | 来源 |
|---|------|------|------------------|------|
| 1 | REST 读(box eu-west-1→CLOB) | `/time` TTFB ~34ms;book GET ~30ms(warm);ICMP ~1.3ms 仅到 Cloudflare 边缘 | ~100% | docs/DEPLOYMENT.md:44-47 |
| 2 | WS 行情推送 | TLS 连接 16ms;`price_change` 推送 ~ms 级 | 稳定 | docs/DEPLOYMENT.md:47, PERFORMANCE-AND-OPS.md:131 |
| 3 | PM 报价对外部信号的跟进滞后 | ~200ms 起步、指数式补齐(非阶跃) | — | backtest/temporal_v2.py, STRATEGY §2 |
| 4 | **taker 买入(FAK)** | 信号→执行 ~20ms(预签名+warm) | **总成功率 3/18=17%**;其中 8/18 "ask 已消失"(FAK-kill),其余为其他 reject;`filled` 字段即最终事实,绝不驻留 | STRATEGY §8(实盘 n=18);commits d127ae2/bb922cc |
| 5 | **GTC 市价化买入 = 孤儿陷阱** | 立即响应 `filled=0` 但**驻留**、异步成交 | 已实盘踩雷 1 次(3 单中 1 单)→ 入场**禁用 GTC**,只用 FAK | STRATEGY §8 |
| 6 | maker 挂单驻留 | 下单即驻留 CLOB;time-to-fill **分布未系统测量** | $80 实盘 ~200 fills/数小时(活跃池);低流量池长期 0 成交 | live-maker 2026-06-28 实盘 |
| 7 | 撤单 | warm 16-19ms / 冷启动 ~34ms(ConnectionWarmer 每 3s 保温);WS 反射撤单触发 ~ms | ~100% | PERFORMANCE-AND-OPS.md:134 |
| 8 | taker 卖出/平仓(薄盘) | maker-exit 在微市场 ~3.5s 成交(小样本) | 薄盘扫 2-8 tick,单次 −$28 级滑损实录 | STRATEGY §7;execution-settlement-lag-root |
| 9 | **买后即卖的可用性** | 链上真实结算 ~2-5s(Polygon);"not enough balance" 多为 CLOB 余额缓存过期(可 `updateBalanceAllowance` 强刷);旧"~25s"是混淆观测,已更正 | — | execution-settlement-lag-root(2026-06-28 更正) |
| 10 | 持仓可见性(data-api /positions) | 滞后 + mark 噪声(幻影 ±$11 级闪烁) | **USDC 是唯一硬底**;风控不得信 positions mark | live-maker 实盘教训 |
| 11 | **taker 费(crypto 微市场)** | — | **0.07·p·(1−p)/股/边**(真实成交反推+验证):p=0.5 → 1.75c/边(往返 3.5c);p=0.9 → 0.63c/边。maker 费 0 + 2026 返佣计划(taker 费的 25%,日结) | STRATEGY §7 🔴 |
| 11b | **温度日盘费用(实查 2026-07-03)** | — | `feeType: weather_fees`,`{rate: 0.05, exponent: 1, takerOnly: true, rebateRate: 0.25}` → **maker 零费 + 收 taker 费 25% 返佣**;taker 付 5% 费率公式 → 垫高知情 taker 扫单门槛,对 maker 结构性利好;我方 taker 平仓也要付(极端价处 p(1−p) 小、费低)。温度桶为 `neg_risk: true`(可 merge/convert) | gamma+clob /markets 实查 |
| 12 | 奖励结算 | 日结;κ = 真实结算/毛估 ≈ 0.237,但测量窗仅 47min 且落在 bug 期,健康 κ 可能 0.3-0.5 | `/rewards/user/total` 为准 | reward-model-calibration + 2026-07-03 审计 |
| 12b | **成交带时间戳(致命陷阱,实证 2026-07-03)** | **data-api `/trades` 的 `timestamp` = Polygon 区块(结算)时间,不是撮合时间:滞后撮合 +2~6s(众数 +3s;拥堵更糟)** | RPC 逐笔核对 12/12 完全等于区块时间戳;此假象曾制造出 +$4.36/窗的假 maker 边(校正后 −$1.59,t=−9.3) | 任何用 tape 时间戳的回测必须先加 2-6s 延迟建模,否则必然说谎 |
| 12c | data-api `/trades` 分页 | — | 新→旧排序 + 静默截断 limit=1000 + offset 硬顶 10000;分页不彻底会制造"安静窗口前视边"(曾造出假 t=5.8) | 必须 offset 分页到穷尽 + 校验每窗完整性 |
| 12d | **CLOB `/data/trades` 顶层字段 = TAKER 视角(幻影库存根源,实证 2026-07-03)** | — | 顶层 `asset_id/size/price` 是 taker 的(对侧 token + taker 总量 + 跨 maker 混合均价);**maker 的真实成交在 `maker_orders[]` 里**(按 `maker_address` 过滤,取 `matched_amount/price/asset_id/side`)。首笔 tail-vendor 实成交实锤:我挂 BUY NO 15@0.953,taker 买 YES 20@0.05,顶层直接记账 = 错 token + 多 5 股(余额差 15×0.953 证伪)。极可能是 2026-06 两次 phantom-inventory/孤立事故的根 | `extract_new_fills(maker_lc)` 已修 + 实录行锁测试;凡按 maker 记账必须走 maker_orders 腿 |
| 13 | 撤单周期(多池串行) | 旧 Python N=45 池 ~104-150s/轮(致命);C++ 重写后见 pmm 基准 | — | PERFORMANCE-AND-OPS.md:41-47 |

## 二、未知项 + 测量方案(按当前活线索排序)

温度日盘 maker 线索(当前唯一活线索)最需要的缺口:

| 缺口 | 为什么关键 | 测量方案 | 成本 |
|------|-----------|---------|------|
| maker time-to-fill 与成交毒性(温度池) | 决定"有奖励但永不成交"还是"成交即被逆选" | **只读可测**:ramp 采样器已在收 book(10min)+trades(1h);fill 速度 ≈ 成交量÷盘内驻留深度 | $0(进行中) |
| 开盘竞争爬坡 L(t) | 决定小资金份额→奖励净值正负号 | 40h 采样器进行中(已从 t+8min 开始) | $0(进行中) |
| 真实 κ(健康 bot) | 奖励折算系数,正类净值的乘数 | **必须真钱**:最小驻留单跑一个奖励日,对账 `/rewards/user/total` | 最小试验的目标之一 |
| 下单→ack 延迟(maker place) | 撤单已测(16-19ms),place 未单独测 | 最小试验首单即测 | 随试验 |
| 结算→赎回→USDC 可用时长(日盘) | 资本周转率:$240 锁多久 | 只读可查(已结算市场的 redeem 时间)+ 试验实测 | $0 + 试验 |
| 温度日盘是否收 taker 费 | 影响对手盘行为与我们的退出成本 | 只读:对照 fee-rate 端点/实测成交 | $0 |
| 挂单被动成交的部分成交粒度 | 部分成交会造成单腿库存 | 试验实测 | 随试验 |

## 三、最小真钱试验协议(按 2026-07-03 新政策)

来自 [scalp 事故] 的铁律:**硬上限必须落在订单数/占用资本上**,不得落在滞后的已实现盈亏上。

1. 目的排序:①补齐上表未知项(place ack、time-to-fill、部分成交、κ、赎回时长);②然后才是净值验证。
2. 上限:PM 最小单(奖励要求 min_size,温度池=50 股 ≈ $10-25);总占用资本硬顶;订单计数硬顶;
   每单都带序列号对账(下单→book 出现→成交/撤销→链上→奖励,全链路时间戳)。
3. 全程监控:每分钟对账 USDC 硬底;任何一步与预期不符(如 GTC 假死、幻影仓)→ 立即停机保全现场。
4. 用户亲自 arm(set_pmkey.sh + PM_TRADER_LIVE),我不代 flip。
