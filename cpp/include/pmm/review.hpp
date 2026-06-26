// pmm/review.hpp — 事后复盘 & 奖励对账 (port of pm_trader/review.py)
//
// 三源 per-market join: 引擎账本(预估奖励+已实现 bleed/库存 P&L) + 事件日志(决策/discovery) +
// 链上实际奖励(data-api /activity?type=REWARD)。核心是把"机器人预估" vs "实际到账"对账——
// 策略份额是 book 快照, PM 按时间加权日结算, 两者会偏离, 只有这个对比能告诉你偏多少。全只读。
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pmm/orders.hpp"  // MakerQuote

namespace pmm::review {

// 实际奖励到账 (data-api /activity?type=REWARD): 每条 {ts, condition_id, usdc, tx}。
// 分页 offset; start/end 是 unix 秒。best-effort: 拿到多少返回多少, 不抛。
[[nodiscard]] std::vector<nlohmann::json> fetch_actual_rewards(const std::string& wallet,
                                                              std::optional<long long> start = std::nullopt,
                                                              std::optional<long long> end = std::nullopt,
                                                              int page = 500, int max_pages = 50);

// 从 events-*.jsonl 加载事件 (可选只取最近 N 天)。
[[nodiscard]] std::vector<nlohmann::json> load_events(const std::string& events_dir,
                                                      std::optional<int> days = std::nullopt);

// 用账本 quotes + 事件 + 实际奖励构建复盘报告。纯函数 (可离线测)。
[[nodiscard]] nlohmann::json summarize(const std::vector<MakerQuote>& quotes,
                                       const std::vector<nlohmann::json>& events,
                                       const std::vector<nlohmann::json>& actual);

// 报告渲染成人类可读文本。
[[nodiscard]] std::string format_report(const nlohmann::json& report);

// 编排: 读账本 + 事件, 拉实际奖励, 返回报告 dict。
[[nodiscard]] nlohmann::json run_review(const std::string& state_dir, const std::string& wallet = "",
                                        int days = 10);

}  // namespace pmm::review
