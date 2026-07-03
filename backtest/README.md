# backtest/ — 调查与研究工具索引

顶层 = **现役**;`archive/` = **已证伪调查**(代码保留供复现/翻案,判决一律以 FINDINGS.md + 记忆为准)。
2026-07-05 起的目录结构;旧文档里的 `backtest/<script>.py` 路径对应 `backtest/archive/<调查>/<script>.py`。

## 现役

| 目录 | 用途 | 状态 |
|---|---|---|
| `deribit_fair/` | **加密尾部高估边**(唯一活边):Deribit 期权隐含公允 vs PM 价;census→deribit_pull→map_markets→paper_ledger 周更;纸面账本状态在 `~/pm-data/tail_ledger.jsonl` | 确认中(上行尾 4-5.5×,18 个月全周期;执行端 = cpp `tail-vendor`) |
| `weather/` | 温度日盘管线(49 城结算语义/METAR 取数/集合预报/偏差校准) | 已证伪(市场胜免费模型,Brier 0.0616 vs 0.0674);保留因翻案路径 = 站点级 MOS 真预报技能 |

## archive/(判决速览)

| 目录 | 调查 | 判决 | 详见 |
|---|---|---|---|
| `copy_trading/` | 复制排行榜赢家 | 死:边在下注尺寸非选题(等权 ROI≤+0.9%),赢单漂移 +3-7c@1-5min;exclude-top 杀死测试本身无效 | FINDINGS §6/§8 |
| `longshot/` | fade 长尾/戏剧性事件 | 死:修正后 maker 变体 +1.0c±2c = 统计零;旧杀死论据全错(删失样本/价差高估/MC 相关性凭空) | FINDINGS §6/§8 |
| `crypto_micro/` | 5/15 分钟微市场(taker 抢单 + maker 做市 + 全部延迟基建) | **双向死**:taker t=−8.5(44 笔链上实录);maker t=−9.3(tape 时间戳=区块时间+3s 校正后);费用为平台定向设计 | FINDINGS §7/§8, STRATEGY_crypto_lag_scalp.md §8 |
| `maker_rewards/` | 流动性奖励做市(宽基/噪声日盘) | 净负~持平:奖励×份额×κ 盖不住逆选;温度日盘变体亦濒死(空盘窗仅 40min + 知情大单) | FINDINGS §2/§8 |
| `arb_scan/` | 跨市/跨盘口/多结果套利扫描 | 死:市场有效,gap < 往返成本 | FINDINGS §2/§6 |

## 数据铁律(所有脚本必须遵守,来源 docs/EXECUTION-MECHANICS.md)

1. data-api `/trades` 的 `timestamp` = Polygon 区块时间(撮合 +2~6s)——亚分钟回测必须建模,否则必然造假边;
2. `/trades` 新→旧 + 静默 limit=1000 截断 + offset 顶 10000——分页必须穷尽并校验每窗完整性;
3. gamma closed 按 endDate 采样是删失分层(offset ~4-5k 顶)——历史队列用 end_date 窗口或 series_id 构造;
4. CLOB prices-history 有 15 天 span 上限——长窗口需拼接;新书 ~0.495 初始化报价需按 book 年龄过滤。
