// pmm/telemetry/dds_publisher.cpp — Fast-DDS 3.x 全量遥测发布实现。
//
// 仅在 WITH_TELEMETRY=ON 时编译 (盒子, 见 ~/dds; CMake 用本文件替掉 no-op publisher.cpp)。
// 设计目标:
//   1) 非阻塞: 每个 DataWriter 用 KEEP_LAST(深 16) + 异步发布 + 极小 max_blocking_time,
//      慢/缺席订阅者绝不回压交易循环 (publish() 至多耗一个 tick)。
//   2) 防崩: 任何 DDS 失败都退化为 no-op —— 构造期异常 → make_publisher() 回退 NoopPublisher;
//      publish() 内吞掉一切异常, 永不向 bot 抛出。
//   3) json→生成 DDS 类型: bot 仍发松散 json (复用现有 event() 风格), 本文件按 IDL 字段名逐字段转换。
//
// QoS 分层: 高频 topic (OrderBookL2 / RawFeed / TradePrint) 用 BEST_EFFORT (绝不阻塞),
//          低频状态 topic 用 RELIABLE (但 max_blocking_time 仅 20ms 兜底, 仍不长阻塞)。
#include "pmm/telemetry/publisher.hpp"

#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>

// 生成的 PubSubType (codegen, include 路径 = ${TGEN}; 内含 telemetry.hpp 的所有结构体)。
#include "telemetryPubSubTypes.hpp"

namespace pmm::telemetry {
namespace {

namespace efd = eprosima::fastdds::dds;

// 每 topic 把松散 json 转成对应生成结构体并 write。无捕获 → 可作函数指针存表。
using WriteFn = void (*)(const nlohmann::json&, efd::DataWriter*);

// json 字段取值 helper (字段名严格对应 IDL; 缺失 → 默认值)。j 为各转换器形参名。
#define GS(k) j.value(k, std::string{})            // string
#define GD(k) j.value(k, 0.0)                       // double
#define GI64(k) j.value(k, static_cast<std::int64_t>(0))  // long long
#define GI32(k) static_cast<std::int32_t>(j.value(k, static_cast<std::int64_t>(0)))  // long
#define GB(k) j.value(k, false)                     // boolean

// 把 json 数组 [{price,size},...] 填入 PriceLevel 序列 (OrderBookL2 的 bids/asks)。
std::vector<PriceLevel> to_levels(const nlohmann::json& arr) {
    std::vector<PriceLevel> out;
    if (!arr.is_array()) return out;
    out.reserve(arr.size());
    for (const auto& lvl : arr) {
        PriceLevel pl;
        pl.price(lvl.value("price", 0.0));
        pl.size(lvl.value("size", 0.0));
        out.push_back(pl);
    }
    return out;
}

// ---- 21 个 topic 的 json→struct 转换器 (字段严格对应 telemetry.idl) ----

void w_Heartbeat(const nlohmann::json& j, efd::DataWriter* w) {
    Heartbeat s;
    s.ts_ms(GI64("ts_ms"));
    s.run_state(GS("run_state"));
    s.uptime_s(GD("uptime_s"));
    s.usdc(GD("usdc"));
    s.equity(GD("equity"));
    s.n_quotes(GI32("n_quotes"));
    s.n_pools_tracked(GI32("n_pools_tracked"));
    s.book_resync_rps(GD("book_resync_rps"));
    static_cast<void>(w->write(&s));
}

void w_QuoteDecision(const nlohmann::json& j, efd::DataWriter* w) {
    QuoteDecision s;
    s.ts_ms(GI64("ts_ms"));
    s.condition_id(GS("condition_id"));
    s.question(GS("question"));
    s.token(GS("token"));
    s.side(GS("side"));
    s.book_seq(GI64("book_seq"));
    s.mid(GD("mid"));
    s.max_spread_c(GD("max_spread_c"));
    s.half_spread_c(GD("half_spread_c"));
    s.sigma_c(GD("sigma_c"));
    s.jump_sigma_c(GD("jump_sigma_c"));
    s.jump_anomaly(GD("jump_anomaly"));
    s.est_reward(GD("est_reward"));
    s.bleed_per_day(GD("bleed_per_day"));
    s.net_per_day(GD("net_per_day"));
    s.existing_qmin(GD("existing_qmin"));
    s.own_qmin(GD("own_qmin"));
    s.share_w(GD("share_w"));
    s.size(GD("size"));
    s.committed_capital(GD("committed_capital"));
    s.tail_var(GD("tail_var"));
    s.net_gate_pass(GB("net_gate_pass"));
    s.tail_capped(GB("tail_capped"));
    s.status(GS("status"));
    s.skip_reason(GS("skip_reason"));
    static_cast<void>(w->write(&s));
}

void w_PoolEval(const nlohmann::json& j, efd::DataWriter* w) {
    PoolEval s;
    s.ts_ms(GI64("ts_ms"));
    s.condition_id(GS("condition_id"));
    s.question(GS("question"));
    s.competitiveness(GD("competitiveness"));
    s.days_to_resolution(GD("days_to_resolution"));
    s.mid(GD("mid"));
    s.volume(GD("volume"));
    s.jump_verdict(GS("jump_verdict"));
    s.empty_band(GB("empty_band"));
    s.vol_mult(GD("vol_mult"));
    s.est_reward(GD("est_reward"));
    s.net_per_day(GD("net_per_day"));
    s.safe_pass(GB("safe_pass"));
    s.comp_pass(GB("comp_pass"));
    s.mid_pass(GB("mid_pass"));
    s.net_pass(GB("net_pass"));
    s.days_pass(GB("days_pass"));
    s.reward_pass(GB("reward_pass"));
    s.selected(GB("selected"));
    s.reject_reason(GS("reject_reason"));
    static_cast<void>(w->write(&s));
}

void w_FillContext(const nlohmann::json& j, efd::DataWriter* w) {
    FillContext s;
    s.ts_ms(GI64("ts_ms"));
    s.condition_id(GS("condition_id"));
    s.question(GS("question"));
    s.token(GS("token"));
    s.side(GS("side"));
    s.size(GD("size"));
    s.price(GD("price"));
    s.leg(GS("leg"));
    s.mid_before(GD("mid_before"));
    s.mid_after(GD("mid_after"));
    s.bleed(GD("bleed"));
    s.book_bid(GD("book_bid"));
    s.book_ask(GD("book_ask"));
    s.book_seq(GI64("book_seq"));
    s.inventory_after(GD("inventory_after"));
    s.reconciled(GB("reconciled"));
    static_cast<void>(w->write(&s));
}

void w_OrderBookL2(const nlohmann::json& j, efd::DataWriter* w) {
    OrderBookL2 s;
    s.ts_ms(GI64("ts_ms"));
    s.condition_id(GS("condition_id"));
    s.token(GS("token"));
    s.seq(GI64("seq"));
    s.source(GS("source"));
    s.update_type(GS("update_type"));
    s.mid(GD("mid"));
    s.inband_qmin(GD("inband_qmin"));
    if (auto it = j.find("bids"); it != j.end()) s.bids(to_levels(*it));
    if (auto it = j.find("asks"); it != j.end()) s.asks(to_levels(*it));
    static_cast<void>(w->write(&s));
}

void w_RawFeed(const nlohmann::json& j, efd::DataWriter* w) {
    RawFeed s;
    s.ts_ms(GI64("ts_ms"));
    s.channel(GS("channel"));
    s.condition_id(GS("condition_id"));
    s.token(GS("token"));
    s.seq(GI64("seq"));
    s.payload(GS("payload"));
    static_cast<void>(w->write(&s));
}

void w_Position(const nlohmann::json& j, efd::DataWriter* w) {
    Position s;
    s.ts_ms(GI64("ts_ms"));
    s.condition_id(GS("condition_id"));
    s.token(GS("token"));
    s.outcome(GS("outcome"));
    s.size(GD("size"));
    s.value(GD("value"));
    static_cast<void>(w->write(&s));
}

void w_EquitySnapshot(const nlohmann::json& j, efd::DataWriter* w) {
    EquitySnapshot s;
    s.ts_ms(GI64("ts_ms"));
    s.usdc(GD("usdc"));
    s.position_value(GD("position_value"));
    s.equity(GD("equity"));
    s.equity_dd_count(GI32("equity_dd_count"));
    s.day_pnl(GD("day_pnl"));
    s.realized_bleed(GD("realized_bleed"));
    s.reward_accrued(GD("reward_accrued"));
    static_cast<void>(w->write(&s));
}

void w_KillEvent(const nlohmann::json& j, efd::DataWriter* w) {
    KillEvent s;
    s.ts_ms(GI64("ts_ms"));
    s.kind(GS("kind"));
    s.reason(GS("reason"));
    static_cast<void>(w->write(&s));
}

void w_RewardSnapshot(const nlohmann::json& j, efd::DataWriter* w) {
    RewardSnapshot s;
    s.ts_ms(GI64("ts_ms"));
    s.daily_reward(GD("daily_reward"));
    s.official_total(GD("official_total"));
    static_cast<void>(w->write(&s));
}

void w_LogEvent(const nlohmann::json& j, efd::DataWriter* w) {
    LogEvent s;
    s.ts_ms(GI64("ts_ms"));
    s.kind(GS("kind"));
    s.condition_id(GS("condition_id"));
    s.latency_ms(GD("latency_ms"));
    s.detail(GS("detail"));
    static_cast<void>(w->write(&s));
}

void w_OrderEvent(const nlohmann::json& j, efd::DataWriter* w) {
    OrderEvent s;
    s.ts_ms(GI64("ts_ms"));
    s.condition_id(GS("condition_id"));
    s.token(GS("token"));
    s.order_id(GS("order_id"));
    s.phase(GS("phase"));
    s.side(GS("side"));
    s.price(GD("price"));
    s.size(GD("size"));
    s.signature_type(GI32("signature_type"));
    s.latency_ms(GD("latency_ms"));
    s.error(GS("error"));
    static_cast<void>(w->write(&s));
}

void w_TradePrint(const nlohmann::json& j, efd::DataWriter* w) {
    TradePrint s;
    s.ts_ms(GI64("ts_ms"));
    s.condition_id(GS("condition_id"));
    s.token(GS("token"));
    s.price(GD("price"));
    s.size(GD("size"));
    s.taker_side(GS("taker_side"));
    s.is_ours(GB("is_ours"));
    static_cast<void>(w->write(&s));
}

void w_FeedHealth(const nlohmann::json& j, efd::DataWriter* w) {
    FeedHealth s;
    s.ts_ms(GI64("ts_ms"));
    s.channel(GS("channel"));
    s.state(GS("state"));
    s.msg_rate(GD("msg_rate"));
    s.gap_count(GI32("gap_count"));
    s.exch_lag_ms(GD("exch_lag_ms"));
    s.n_subscribed(GI32("n_subscribed"));
    static_cast<void>(w->write(&s));
}

void w_LatencyProfile(const nlohmann::json& j, efd::DataWriter* w) {
    LatencyProfile s;
    s.ts_ms(GI64("ts_ms"));
    s.place_ms(GD("place_ms"));
    s.cancel_ms(GD("cancel_ms"));
    s.chain_query_ms(GD("chain_query_ms"));
    s.loop_tick_ms(GD("loop_tick_ms"));
    s.book_staleness_ms(GD("book_staleness_ms"));
    static_cast<void>(w->write(&s));
}

void w_ChainState(const nlohmann::json& j, efd::DataWriter* w) {
    ChainState s;
    s.ts_ms(GI64("ts_ms"));
    s.token(GS("token"));
    s.chain_size(GD("chain_size"));
    s.usdc(GD("usdc"));
    s.allowance(GD("allowance"));
    s.settlement_lag_ms(GD("settlement_lag_ms"));
    s.pending_inflight(GI32("pending_inflight"));
    static_cast<void>(w->write(&s));
}

void w_RiskState(const nlohmann::json& j, efd::DataWriter* w) {
    RiskState s;
    s.ts_ms(GI64("ts_ms"));
    s.kill_armed(GB("kill_armed"));
    s.equity_dd_count(GI32("equity_dd_count"));
    s.churn_count(GI32("churn_count"));
    s.chain_flat_guard(GB("chain_flat_guard"));
    s.capital_deployed(GD("capital_deployed"));
    s.tail_budget_used(GD("tail_budget_used"));
    s.drawdown(GD("drawdown"));
    static_cast<void>(w->write(&s));
}

void w_DiscoveryScan(const nlohmann::json& j, efd::DataWriter* w) {
    DiscoveryScan s;
    s.ts_ms(GI64("ts_ms"));
    s.n_scanned(GI32("n_scanned"));
    s.n_safe(GI32("n_safe"));
    s.n_dropped_jumpy(GI32("n_dropped_jumpy"));
    s.n_dropped_depleted(GI32("n_dropped_depleted"));
    s.n_dropped_extreme(GI32("n_dropped_extreme"));
    s.n_dropped_comp(GI32("n_dropped_comp"));
    s.n_selected(GI32("n_selected"));
    s.scan_duration_ms(GD("scan_duration_ms"));
    static_cast<void>(w->write(&s));
}

void w_ConfigSnapshot(const nlohmann::json& j, efd::DataWriter* w) {
    ConfigSnapshot s;
    s.ts_ms(GI64("ts_ms"));
    s.git_commit(GS("git_commit"));
    s.capital(GD("capital"));
    s.max_loss(GD("max_loss"));
    s.jump_vol_weight(GD("jump_vol_weight"));
    s.max_competitiveness(GD("max_competitiveness"));
    s.max_pool_frac(GD("max_pool_frac"));
    s.tail_budget(GD("tail_budget"));
    s.extra_json(GS("extra_json"));
    static_cast<void>(w->write(&s));
}

void w_SystemHealth(const nlohmann::json& j, efd::DataWriter* w) {
    SystemHealth s;
    s.ts_ms(GI64("ts_ms"));
    s.cpu_pct(GD("cpu_pct"));
    s.mem_mb(GD("mem_mb"));
    s.http_req_rate(GD("http_req_rate"));
    s.rate_limit_hits(GI32("rate_limit_hits"));
    s.db_write_ms(GD("db_write_ms"));
    s.thread_count(GI32("thread_count"));
    static_cast<void>(w->write(&s));
}

void w_PoolReward(const nlohmann::json& j, efd::DataWriter* w) {
    PoolReward s;
    s.ts_ms(GI64("ts_ms"));
    s.condition_id(GS("condition_id"));
    s.question(GS("question"));
    s.est_reward(GD("est_reward"));
    s.realized_reward(GD("realized_reward"));
    s.share(GD("share"));
    static_cast<void>(w->write(&s));
}

#undef GS
#undef GD
#undef GI64
#undef GI32
#undef GB

// ---- DdsPublisher: 一个 participant + 一个 publisher + 每 topic 一个 writer ----
class DdsPublisher final : public Publisher {
public:
    DdsPublisher() {
        participant_ = efd::DomainParticipantFactory::get_instance()->create_participant(
            0, efd::PARTICIPANT_QOS_DEFAULT);
        if (participant_ == nullptr) {
            throw std::runtime_error("DDS: create_participant failed");
        }
        dds_pub_ = participant_->create_publisher(efd::PUBLISHER_QOS_DEFAULT);
        if (dds_pub_ == nullptr) {
            throw std::runtime_error("DDS: create_publisher failed");
        }

        // 高频 topic → BEST_EFFORT (绝不阻塞), 其余 → RELIABLE (但短 max_blocking_time 兜底)。
        register_topic<HeartbeatPubSubType>(topic::kHeartbeat, &w_Heartbeat, /*reliable=*/true);
        register_topic<QuoteDecisionPubSubType>(topic::kQuoteDecision, &w_QuoteDecision, true);
        register_topic<PoolEvalPubSubType>(topic::kPoolEval, &w_PoolEval, true);
        register_topic<FillContextPubSubType>(topic::kFillContext, &w_FillContext, true);
        register_topic<OrderBookL2PubSubType>(topic::kOrderBookL2, &w_OrderBookL2, /*reliable=*/false);
        register_topic<RawFeedPubSubType>(topic::kRawFeed, &w_RawFeed, /*reliable=*/false);
        register_topic<PositionPubSubType>(topic::kPosition, &w_Position, true);
        register_topic<EquitySnapshotPubSubType>(topic::kEquitySnapshot, &w_EquitySnapshot, true);
        register_topic<KillEventPubSubType>(topic::kKillEvent, &w_KillEvent, true);
        register_topic<RewardSnapshotPubSubType>(topic::kRewardSnapshot, &w_RewardSnapshot, true);
        register_topic<LogEventPubSubType>(topic::kLogEvent, &w_LogEvent, true);
        register_topic<OrderEventPubSubType>(topic::kOrderEvent, &w_OrderEvent, true);
        register_topic<TradePrintPubSubType>(topic::kTradePrint, &w_TradePrint, /*reliable=*/false);
        register_topic<FeedHealthPubSubType>(topic::kFeedHealth, &w_FeedHealth, true);
        register_topic<LatencyProfilePubSubType>(topic::kLatencyProfile, &w_LatencyProfile, true);
        register_topic<ChainStatePubSubType>(topic::kChainState, &w_ChainState, true);
        register_topic<RiskStatePubSubType>(topic::kRiskState, &w_RiskState, true);
        register_topic<DiscoveryScanPubSubType>(topic::kDiscoveryScan, &w_DiscoveryScan, true);
        register_topic<ConfigSnapshotPubSubType>(topic::kConfigSnapshot, &w_ConfigSnapshot, true);
        register_topic<SystemHealthPubSubType>(topic::kSystemHealth, &w_SystemHealth, true);
        register_topic<PoolRewardPubSubType>(topic::kPoolReward, &w_PoolReward, true);
    }

    ~DdsPublisher() override {
        if (participant_ != nullptr) {
            participant_->delete_contained_entities();
            efd::DomainParticipantFactory::get_instance()->delete_participant(participant_);
        }
    }

    DdsPublisher(const DdsPublisher&) = delete;
    DdsPublisher& operator=(const DdsPublisher&) = delete;
    DdsPublisher(DdsPublisher&&) = delete;
    DdsPublisher& operator=(DdsPublisher&&) = delete;

    void publish(const std::string& topic, const nlohmann::json& data) override {
        // 防崩: 整个发布路径包在 try 内, 任何 DDS/json 异常都吞掉, 永不抛回 bot。
        try {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = writers_.find(topic);
            if (it == writers_.end()) return;  // 未知 topic → 静默丢弃
            it->second.convert(data, it->second.writer);
        } catch (...) {
            // 遥测是 best-effort, 失败绝不影响交易循环。
        }
    }

private:
    struct Entry {
        efd::DataWriter* writer;
        WriteFn convert;
    };

    template <typename PubSubT>
    void register_topic(const char* name, WriteFn fn, bool reliable) {
        efd::TypeSupport ts(new PubSubT());
        ts.register_type(participant_);  // 同时注册嵌套类型 (如 OrderBookL2 的 PriceLevel)

        efd::Topic* tp = participant_->create_topic(name, ts.get_type_name(), efd::TOPIC_QOS_DEFAULT);
        if (tp == nullptr) {
            throw std::runtime_error(std::string("DDS: create_topic failed: ") + name);
        }

        efd::DataWriterQos wq = efd::DATAWRITER_QOS_DEFAULT;
        // KEEP_LAST(16): 历史有界, 慢订阅者只丢旧样本, 不无限堆积。
        wq.history().kind = efd::KEEP_LAST_HISTORY_QOS;
        wq.history().depth = 16;
        // 异步发布: write() 把样本交给后台发送线程后立即返回, 不在交易线程上做网络 IO。
        wq.publish_mode().kind = efd::ASYNCHRONOUS_PUBLISH_MODE;
        wq.reliability().kind = reliable ? efd::RELIABLE_RELIABILITY_QOS : efd::BEST_EFFORT_RELIABILITY_QOS;
        // RELIABLE 历史满时最多阻塞 20ms (默认 100ms) —— 兜底, 绝不长回压交易循环。
        wq.reliability().max_blocking_time = efd::Duration_t{0, 20000000u};

        efd::DataWriter* dw = dds_pub_->create_datawriter(tp, wq);
        if (dw == nullptr) {
            throw std::runtime_error(std::string("DDS: create_datawriter failed: ") + name);
        }
        writers_.emplace(name, Entry{dw, fn});
    }

    std::mutex mu_;
    efd::DomainParticipant* participant_ = nullptr;
    efd::Publisher* dds_pub_ = nullptr;
    std::unordered_map<std::string, Entry> writers_;
};

}  // namespace

// 工厂: 尝试建 DdsPublisher; 任何构造失败 (participant/publisher/writer) → 退化 NoopPublisher。
std::unique_ptr<Publisher> make_publisher() {
    try {
        return std::make_unique<DdsPublisher>();
    } catch (...) {
        return std::make_unique<NoopPublisher>();
    }
}

}  // namespace pmm::telemetry
