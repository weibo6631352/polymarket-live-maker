// pmm/telemetry/publisher.hpp — 全量遥测发布接口
//
// bot 各事件点调 publish(topic, json) → Fast-DDS topic。json 载荷复用 bot 现有 event() 风格,
// 与 DDS codegen 解耦 (DdsPublisher 内部把 json 转成生成的 DDS 类型)。
// 两实现:
//   - NoopPublisher: 无 Fast-DDS 时 (Mac 本地构建/单测), 零开销空操作;
//   - DdsPublisher:  盒子上 WITH_TELEMETRY=ON (见 dds_publisher.cpp), 转 DDS 类型并发布。
#pragma once

#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace pmm::telemetry {

// topic 名常量 (对应 telemetry.idl 的 struct)。集中一处避免拼写漂移。
namespace topic {
inline constexpr const char* kHeartbeat = "Heartbeat";
inline constexpr const char* kQuoteDecision = "QuoteDecision";
inline constexpr const char* kPoolEval = "PoolEval";
inline constexpr const char* kFillContext = "FillContext";
inline constexpr const char* kOrderBookL2 = "OrderBookL2";
inline constexpr const char* kRawFeed = "RawFeed";
inline constexpr const char* kPosition = "Position";
inline constexpr const char* kEquitySnapshot = "EquitySnapshot";
inline constexpr const char* kKillEvent = "KillEvent";
inline constexpr const char* kRewardSnapshot = "RewardSnapshot";
inline constexpr const char* kLogEvent = "LogEvent";
inline constexpr const char* kOrderEvent = "OrderEvent";
inline constexpr const char* kTradePrint = "TradePrint";
inline constexpr const char* kFeedHealth = "FeedHealth";
inline constexpr const char* kLatencyProfile = "LatencyProfile";
inline constexpr const char* kChainState = "ChainState";
inline constexpr const char* kRiskState = "RiskState";
inline constexpr const char* kDiscoveryScan = "DiscoveryScan";
inline constexpr const char* kConfigSnapshot = "ConfigSnapshot";
inline constexpr const char* kSystemHealth = "SystemHealth";
inline constexpr const char* kPoolReward = "PoolReward";
inline constexpr const char* kCuratorCommand = "CuratorCommand";  // host→bot 控制 (唯一反向 topic)
}  // namespace topic

class Publisher {
public:
    virtual ~Publisher() = default;
    // 发布一条遥测到指定 topic。线程安全 (实现需自保证)。data 字段对应该 topic 的 IDL struct。
    virtual void publish(const std::string& topic, const nlohmann::json& data) = 0;

    // 外部 curator 经 DDS CuratorCommand 推白名单 → bot 收到时回调 (whitelist_csv = 逗号分隔 cond)。
    // host→bot 反向通道。NoopPublisher 默认空操作; DdsPublisher 建 DataReader 实现。线程安全。
    using CommandCallback = std::function<void(const std::string& whitelist_csv)>;
    virtual void set_command_callback(CommandCallback) {}
};

// 零开销空操作: 无 Fast-DDS 构建 / 显式禁用时用。inline 头内, DdsPublisher 失败可回退到它。
class NoopPublisher final : public Publisher {
public:
    void publish(const std::string& /*topic*/, const nlohmann::json& /*data*/) override {}
};

// 工厂: WITH_TELEMETRY=ON 且 Fast-DDS 可用 → DdsPublisher; 否则 NoopPublisher。
// 实现按构建开关分两个 .cpp (publisher.cpp / dds_publisher.cpp), CMake 二选一编译。
[[nodiscard]] std::unique_ptr<Publisher> make_publisher();

}  // namespace pmm::telemetry
