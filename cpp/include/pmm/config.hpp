// pmm/config.hpp — 运行配置 (port of runner.py RunnerConfig)
//
// 字段、默认值、env 名称与 Python RunnerConfig.from_env 一一对应。
#pragma once

#include <string>

namespace pmm {

struct RunnerConfig {
    bool live{false};
    // 非 live 时: dry_live=true 演练 LIVE 代码路径 (accrue_maker_rewards_live + DryRunSubmitter
    //   记录订单不发送); dry_live=false 用 PAPER 模拟器。live=true 永远优先。
    bool dry_live{true};
    double capital{200.0};
    double min_daily{80.0};
    int scan_top{0};  // 0 = 扫描所有合格池 (不设上限)
    int half_spread_ticks{1};
    bool use_optimal_spread{false};
    // micro-price 中心化: 47K 信号实测 β≈0.001 (mid 受 tick 量化, 极少移动; 领先只预测罕见 tick 跨越的
    // 方向 84%, 但幅度≈0)。最优偏移 = β×lead ≈ 0; 在 99.3% 不动样本上偏移会吃 binding-min 奖励代价 →
    // 净负。故默认关 (数据不支持)。保留 flag+代码, 行情变了可 LM_MICRO_CENTER=1 重启。
    bool micro_center{false};
    double micro_gate_c{0.3};   // 触发门 (¢): 启用时只在最强信号 (84% 方向) 才动
    double micro_beta{0.1};     // 启用时的保守偏移 (实测 β 远低于早先猜的 0.5)
    // 利润模型校准 κ: 真实结算/毛估 (实测 6-28 ≈ 0.237, 毛估高估 4.2×; 来自盘口快照低估真实竞争)。
    // 把竞争对手分 existing_qmin 充气 1/κ → 估计份额/奖励降到真实水平 → 最优半宽变宽 (少被逆选)。
    double reward_calib{0.237};
    bool waterfill{true};       // 注水配资 (边际 κ×奖励/$ 均衡; 取代 capital∝score)
    // 竞争交给盘口 (book_inband_qmin 直接测) + 净边际门 + 跳变 σ; 不再用 market_competitiveness (冗余代理)。
    double extreme_mid_margin{0.2};   // 剔除 mid<0.2 或 >0.8 的近极端价池 (逆选/趋势源: South Korea)
    double jump_vol_weight{0.7};      // 跳变感知 bleed: 挂宽到"罕见成交" (奖励来自挂在带里, 成交是纯成本)。
                                      // 0.4 实测把 spread 收太紧 → 30min 11 笔成交 churn → 净亏; 利润靠多池/大size, 非紧 spread
    bool net_edge_gate{true};         // 净边际门: 跳变感知 net=reward-bleed ≤ 0 的池不报价 (毒池自然出局)
    double tail_budget{20.0};         // 尾部-VaR: 单次成交最坏损失 ≤ 此 $ 额 (bug 修后逆选有界, 放宽→更大 size/更多池)
    double tail_k_sigma{3.0};         // 最坏跳变 = k×σ (3σ 尾部); 与 jump_vol_weight 配合估单次成交尾部
    int recenter_ticks{1};
    double min_days_to_resolution{10.0};  // 剔除 N 天内结算的池 (近结算=催化剂风险); 0=关
    double max_vol_mult{2.5};  // 剔除 实现日波动 > N×奖励带宽 的跳池; 0=关
    double crossing_cost_c{0.0};
    double risk_tolerance_days{7.0};
    int max_token_overlap{1};
    double poll_seconds{60.0};
    double discovery_interval_s{600.0};
    int cooldown_rounds{3};
    double max_loss{20.0};
    double min_wallet_usdc{0.0};
    bool reeval_enabled{true};
    double reeval_interval_s{300.0};
    double min_pool_reward{0.5};
    double size_share_cap{0.33};
    double quality_floor_frac{0.10};
    double max_pool_frac{0.25};
    double order_expiry_s{0.0};
    double chop_aversion{0.7};
    double max_mid_vel_cps{4.0};
    double min_hold_s{600.0};
    std::string state_dir{"state"};
    std::string kill_file{"KILL"};
    int retention_days{30};
    bool events_enabled{true};
    double event_poll_every_s{10.0};
    double stats_every_s{60.0};
    double max_req_per_sec{149.0};
    bool ws_enabled{false};  // dataclass 默认 false (单测无网络); from_env 打开
    int resync_workers{8};

    // 读取进程环境 (+ 项目本地 .env) 构造配置。
    static RunnerConfig from_env();

    // 启动横幅 (mode/capital/poll/min_daily/max_loss)。
    [[nodiscard]] std::string banner() const;
};

}  // namespace pmm
