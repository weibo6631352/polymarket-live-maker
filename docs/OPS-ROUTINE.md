# tail-vendor 试验运营手册(2026-07-04 起)

## 常态(自动)
- **bot**:`tail-vendor-trial` unit(开机自启,Restart=on-failure,3 连拒 halt 不复活);每 300s 扫描,
  白名单热读,实时盘口定价压墙前,持久账本 `state/tail_vendor_held.json`。
- **监控**:会话内 Monitor 每 10min 巡检双单元(fills/halt/error/心跳失联/单元死亡,期望态文件
  `tv_expected.txt`);自主循环 ~1h 兜底心跳。
- **持久看门狗**(会话外,2026-07-07 起):`tv-watchdog.timer` 每 5min 跑 `cpp/deploy/tv_watchdog.sh` —
  尊重 STOP flag;真业务拒单 halt 告警不拉起(保全现场);其它 down/卡死(心跳>15min)限速自动拉起
  (≤4/h 超则闩锁);白名单>90min 未刷新告警。每轮写 `state/tv_watchdog_status.json`(pmpull 一眼看健康:
  active/hb_age_s/wl_age_s/verdict/action)。补 systemd `Restart=on-failure` 不救 clean-exit(code 0) 的洞。
- **急停**:`touch /root/polymarket-live-maker/STOP_TAIL_VENDOR`(1s 撤全部+退出)。

## 每日(会话循环里做,全零成本)
1. **刷新策展白名单**:`backtest/deribit_fair/push_whitelist.sh`(census→deribit→map→rank→推箱)。
   边际口径(2026-07-07 起 **只做 BTC/ETH 直接 Deribit 锚**):SOL/XRP 短期尾部无期权锚(粗糙历史
   比率 + 高相关簇风险 + 挤占干净锚定盘),已在 make_whitelist 关闭(`INCLUDE_UNANCHORED=False`,代码保留)。
   已成交的 SOL/XRP 老仓持有到结算(不强平);挂单在新名单外自动撤(left_window)。在场单若仍在新名单内不受影响。
   **完整性闸门(FAIL-CLOSED)**:每小时刷新前 `validate_curation.py` 逐级校验 census/deribit/map 两币齐全+量在带内、
   新名单不相对上版坍缩(<40%);任一级"信息不完整"就非零退出 → 不覆盖 → 保留上一版好名单(宁用旧而全,不用新而残)。
   持续失败会让白名单变旧 → tv-watchdog 在 >90min 时告警。
2. **记分板**:`pmpull .../tail_vendor_log.jsonl ~/pm-data/ && python3 backtest/deribit_fair/live_ledger.py`
   (领先指标=入场 EV;fair 漂移在 fairY 列)。
3. **账目对账**:USDC 变化必须能被成交/锁定逐笔解释(balance-check;差异>$0.5 即查)。

## 每周
- `paper_ledger.py`(标记 + 按规则开新纸面仓;5 结算/0 触及 @ 07-04)。

## 结算周运维(7/6 起,每个结算日 12:00Z 后)
1. **确认结算**:gamma `/markets?slug=` → closed + outcomePrices。
2. **记分**:live_ledger 自动把 RESOLVED 行计入 realized P&L 与命中数(判据:累计正 + 命中 ≤ Poisson+2σ)。
3. **释放 held 上限**:结算后该 token 的抵押风险已定,但 USDC 未回(PM 不自动赎回)。
   停 unit → 编辑 `state/tail_vendor_held.json`(减去该 token 的 held_total/held_coin,
   token_coin/token_note 保留归因)→ 起 unit。用 base64 通道写(pmbox 引号会咬)。
4. **赎回(拿回 USDC)**:negRisk=False 标准二元 → CTF redeemPositions。当前无程序化通道
   (magic-proxy 需 PM relayer),**试验期用 PM 网页 Claim 手动领**;若持续运营再造赎回 tx 机器。
5. **BTC-66k 特别注**:若 7/8 YES 命中(BTC>66k),15 股 NO 归零,live_ledger 会把它计成
   预期内尾部命中——单仓亏损 ≤ $14.30,属校准方差,不触发策略层动作。

## 已判死/停用(勿复活)
- `pmm-temp-trial`:KILL_LATCH + disabled。温度做市 07-04 终审死(FINDINGS §8)。
  复活条件 = 真预报边或离线证明的 reward-dominant 配置。
- 共享账户铁律:任何新策略上 0x78dE 必须 `LM_ORPHAN_SWEEP=0`(或等效自限清扫)。

## 交易不变量(2026-07-06 事故驱动;任何新准入/新功能必须逐条对照)

事故分类学:两次真金亏损(温度 −$46.7、clamp ITM −$23)全是**授权边界洞**,不是执行 bug:
① 授权比验证宽(温度:授权 33 桶,只盯了 1 个);② 缺失数据默认放行(clamp:空 bid 当 0 过检)。
外加 4 次"外部数据语义"陷阱(taker 视角、closed 删失×2、区块时间戳)。

**不变量清单(分层执守):**
1. 授权层(curator/whitelist):每个 slug 必须有独立锚 —— Deribit fair(≤2%)或 执行价≥现货×1.05
   (首卖 ×1.08 + 可见 bid≤4c);缺数据 = 拒绝,永不默认放行;
2. 决策层(decide,纯函数):2-7c 带、不越 bid、不排墙后、四层资金上限(单笔/市场/币/总)、1-6 天窗;
3. 执行层:下单前 would_cross 实时复核;3 连拒 halt;
4. 对账层:成交只认 maker_orders 腿;持久账本+游标;结算释放走 settle_release(closed=true 回查);
5. 监控层:双单元存活/心跳失联/成交/halt + **ITM 持仓看门狗**(持有执行价<现货的 NO = 授权又漏,1h 一查);
6. bot 端白名单 FAIL-CLOSED:文件缺失/损坏/空 → 本轮不交易(绝不退化为无策展交易)。

架构原则映射(高内聚低耦合):授权只在 curator(改一处生效);定价/风控只在纯函数核(可单测);
IO/账本只在 app;监控独立于交易进程。新策略接入必须:独立 unit + LM_ORPHAN_SWEEP=0 + 自己的授权锚。
