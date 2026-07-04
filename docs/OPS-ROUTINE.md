# tail-vendor 试验运营手册(2026-07-04 起)

## 常态(自动)
- **bot**:`tail-vendor-trial` unit(开机自启,Restart=on-failure,3 连拒 halt 不复活);每 300s 扫描,
  白名单热读,实时盘口定价压墙前,持久账本 `state/tail_vendor_held.json`。
- **监控**:会话内 Monitor 每 10min 巡检双单元(fills/halt/error/心跳失联/单元死亡,期望态文件
  `tv_expected.txt`);自主循环 ~1h 兜底心跳。
- **急停**:`touch /root/polymarket-live-maker/STOP_TAIL_VENDOR`(1s 撤全部+退出)。

## 每日(会话循环里做,全零成本)
1. **刷新策展白名单**:`backtest/deribit_fair/push_whitelist.sh`(census→deribit→map→rank→推箱)。
   边际口径:BTC/ETH 用 Deribit 锚,SOL/XRP 用历史比率 10.8x/6.4x。在场单若仍在新名单内不受影响。
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
