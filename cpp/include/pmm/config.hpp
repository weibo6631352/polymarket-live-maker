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
    bool micro_center{true};    // micro-price 中心化 (预测策略, 门控+保守)
    double micro_gate_c{0.2};   // 触发门 (¢): micro-price 领先 >= 此值才偏移
    double micro_beta{0.5};     // 偏移领先的比例
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
