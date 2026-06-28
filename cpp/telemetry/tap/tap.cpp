// telemetry/tap/tap.cpp — Fast-DDS 订阅者 → stdout JSON 行 (web 仪表盘 / 记录器的上游).
//
// 仅 WITH_TELEMETRY=ON 编译。对 21 个 topic 各建一个 DataReader; 收到样本 → 生成结构体反序列化为
// nlohmann::json (publisher 转换器的逆), 包成 {"topic":"<Name>","data":{...}} 打一行紧凑 JSON 到 stdout
// 并 flush。下游: ws_server.py (广播给浏览器) + recorder.py (落 SQLite)。
//
// 这是纯消费侧: 不碰交易逻辑, 不发布。BEST_EFFORT reader (遥测尽力而为, 不回压发布者)。
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>

#include "pmm/telemetry/publisher.hpp"  // topic:: 常量 (避免名字漂移)
#include "telemetryPubSubTypes.hpp"     // 生成的 PubSubTypes + 结构体 (pmm::telemetry::*)

namespace efd = eprosima::fastdds::dds;
using nlohmann::json;
using namespace pmm::telemetry;  // 生成的结构体类型 Heartbeat/QuoteDecision/...

namespace {

std::mutex g_out_mu;                 // 串行化 stdout (多 reader 线程并发回调)
std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop.store(true); }

// 价档序列 → JSON 数组 [{price,size},...]
json levels_to_json(const std::vector<PriceLevel>& lv) {
    json a = json::array();
    for (const auto& l : lv) a.push_back(json{{"price", l.price()}, {"size", l.size()}});
    return a;
}

// ---- 21 个 struct→json 转换器 (publisher 端 json→struct 的逆; 字段严格对应 telemetry.idl) ----

json j_Heartbeat(const Heartbeat& s) {
    return json{{"ts_ms", s.ts_ms()}, {"run_state", s.run_state()}, {"uptime_s", s.uptime_s()},
                {"usdc", s.usdc()}, {"equity", s.equity()}, {"n_quotes", s.n_quotes()},
                {"n_pools_tracked", s.n_pools_tracked()}, {"book_resync_rps", s.book_resync_rps()}};
}

json j_QuoteDecision(const QuoteDecision& s) {
    return json{{"ts_ms", s.ts_ms()}, {"condition_id", s.condition_id()}, {"question", s.question()},
                {"token", s.token()}, {"side", s.side()}, {"book_seq", s.book_seq()}, {"mid", s.mid()},
                {"max_spread_c", s.max_spread_c()}, {"half_spread_c", s.half_spread_c()},
                {"sigma_c", s.sigma_c()}, {"jump_sigma_c", s.jump_sigma_c()},
                {"jump_anomaly", s.jump_anomaly()}, {"est_reward", s.est_reward()},
                {"bleed_per_day", s.bleed_per_day()}, {"net_per_day", s.net_per_day()},
                {"existing_qmin", s.existing_qmin()}, {"own_qmin", s.own_qmin()},
                {"share_w", s.share_w()}, {"size", s.size()}, {"committed_capital", s.committed_capital()},
                {"tail_var", s.tail_var()}, {"net_gate_pass", s.net_gate_pass()},
                {"tail_capped", s.tail_capped()}, {"status", s.status()}, {"skip_reason", s.skip_reason()}};
}

json j_PoolEval(const PoolEval& s) {
    return json{{"ts_ms", s.ts_ms()}, {"condition_id", s.condition_id()}, {"question", s.question()},
                {"competitiveness", s.competitiveness()}, {"days_to_resolution", s.days_to_resolution()},
                {"mid", s.mid()}, {"volume", s.volume()},
                {"reward_rate_per_day", s.reward_rate_per_day()}, {"volume_24hr", s.volume_24hr()},
                {"min_capital", s.min_capital()},
                {"jump_verdict", s.jump_verdict()},
                {"empty_band", s.empty_band()}, {"vol_mult", s.vol_mult()}, {"est_reward", s.est_reward()},
                {"net_per_day", s.net_per_day()}, {"safe_pass", s.safe_pass()}, {"comp_pass", s.comp_pass()},
                {"mid_pass", s.mid_pass()}, {"net_pass", s.net_pass()}, {"days_pass", s.days_pass()},
                {"reward_pass", s.reward_pass()}, {"selected", s.selected()},
                {"reject_reason", s.reject_reason()}};
}

json j_FillContext(const FillContext& s) {
    return json{{"ts_ms", s.ts_ms()}, {"condition_id", s.condition_id()}, {"question", s.question()},
                {"token", s.token()}, {"side", s.side()}, {"size", s.size()}, {"price", s.price()},
                {"leg", s.leg()}, {"mid_before", s.mid_before()}, {"mid_after", s.mid_after()},
                {"bleed", s.bleed()}, {"book_bid", s.book_bid()}, {"book_ask", s.book_ask()},
                {"book_seq", s.book_seq()}, {"inventory_after", s.inventory_after()},
                {"reconciled", s.reconciled()}};
}

json j_OrderBookL2(const OrderBookL2& s) {
    return json{{"ts_ms", s.ts_ms()}, {"condition_id", s.condition_id()}, {"token", s.token()},
                {"seq", s.seq()}, {"source", s.source()}, {"update_type", s.update_type()},
                {"mid", s.mid()}, {"inband_qmin", s.inband_qmin()},
                {"bids", levels_to_json(s.bids())}, {"asks", levels_to_json(s.asks())}};
}

json j_RawFeed(const RawFeed& s) {
    return json{{"ts_ms", s.ts_ms()}, {"channel", s.channel()}, {"condition_id", s.condition_id()},
                {"token", s.token()}, {"seq", s.seq()}, {"payload", s.payload()}};
}

json j_Position(const Position& s) {
    return json{{"ts_ms", s.ts_ms()}, {"condition_id", s.condition_id()}, {"token", s.token()},
                {"outcome", s.outcome()}, {"size", s.size()}, {"value", s.value()}};
}

json j_EquitySnapshot(const EquitySnapshot& s) {
    return json{{"ts_ms", s.ts_ms()}, {"usdc", s.usdc()}, {"position_value", s.position_value()},
                {"equity", s.equity()}, {"equity_dd_count", s.equity_dd_count()}, {"day_pnl", s.day_pnl()},
                {"realized_bleed", s.realized_bleed()}, {"reward_accrued", s.reward_accrued()}};
}

json j_KillEvent(const KillEvent& s) {
    return json{{"ts_ms", s.ts_ms()}, {"kind", s.kind()}, {"reason", s.reason()}};
}

json j_RewardSnapshot(const RewardSnapshot& s) {
    return json{{"ts_ms", s.ts_ms()}, {"daily_reward", s.daily_reward()},
                {"official_total", s.official_total()}};
}

json j_LogEvent(const LogEvent& s) {
    return json{{"ts_ms", s.ts_ms()}, {"kind", s.kind()}, {"condition_id", s.condition_id()},
                {"latency_ms", s.latency_ms()}, {"detail", s.detail()}};
}

json j_OrderEvent(const OrderEvent& s) {
    return json{{"ts_ms", s.ts_ms()}, {"condition_id", s.condition_id()}, {"token", s.token()},
                {"order_id", s.order_id()}, {"phase", s.phase()}, {"side", s.side()}, {"price", s.price()},
                {"size", s.size()}, {"signature_type", s.signature_type()}, {"latency_ms", s.latency_ms()},
                {"error", s.error()}};
}

json j_TradePrint(const TradePrint& s) {
    return json{{"ts_ms", s.ts_ms()}, {"condition_id", s.condition_id()}, {"token", s.token()},
                {"price", s.price()}, {"size", s.size()}, {"taker_side", s.taker_side()},
                {"is_ours", s.is_ours()}};
}

json j_FeedHealth(const FeedHealth& s) {
    return json{{"ts_ms", s.ts_ms()}, {"channel", s.channel()}, {"state", s.state()},
                {"msg_rate", s.msg_rate()}, {"gap_count", s.gap_count()}, {"exch_lag_ms", s.exch_lag_ms()},
                {"n_subscribed", s.n_subscribed()}};
}

json j_LatencyProfile(const LatencyProfile& s) {
    return json{{"ts_ms", s.ts_ms()}, {"place_ms", s.place_ms()}, {"cancel_ms", s.cancel_ms()},
                {"chain_query_ms", s.chain_query_ms()}, {"loop_tick_ms", s.loop_tick_ms()},
                {"book_staleness_ms", s.book_staleness_ms()}};
}

json j_ChainState(const ChainState& s) {
    return json{{"ts_ms", s.ts_ms()}, {"token", s.token()}, {"chain_size", s.chain_size()},
                {"usdc", s.usdc()}, {"allowance", s.allowance()},
                {"settlement_lag_ms", s.settlement_lag_ms()}, {"pending_inflight", s.pending_inflight()}};
}

json j_RiskState(const RiskState& s) {
    return json{{"ts_ms", s.ts_ms()}, {"kill_armed", s.kill_armed()},
                {"equity_dd_count", s.equity_dd_count()}, {"churn_count", s.churn_count()},
                {"chain_flat_guard", s.chain_flat_guard()}, {"capital_deployed", s.capital_deployed()},
                {"tail_budget_used", s.tail_budget_used()}, {"drawdown", s.drawdown()}};
}

json j_DiscoveryScan(const DiscoveryScan& s) {
    return json{{"ts_ms", s.ts_ms()}, {"n_scanned", s.n_scanned()}, {"n_safe", s.n_safe()},
                {"n_dropped_jumpy", s.n_dropped_jumpy()}, {"n_dropped_depleted", s.n_dropped_depleted()},
                {"n_dropped_extreme", s.n_dropped_extreme()}, {"n_dropped_comp", s.n_dropped_comp()},
                {"n_selected", s.n_selected()}, {"scan_duration_ms", s.scan_duration_ms()}};
}

json j_ConfigSnapshot(const ConfigSnapshot& s) {
    return json{{"ts_ms", s.ts_ms()}, {"git_commit", s.git_commit()}, {"capital", s.capital()},
                {"max_loss", s.max_loss()}, {"jump_vol_weight", s.jump_vol_weight()},
                {"max_competitiveness", s.max_competitiveness()}, {"max_pool_frac", s.max_pool_frac()},
                {"tail_budget", s.tail_budget()}, {"extra_json", s.extra_json()}};
}

json j_SystemHealth(const SystemHealth& s) {
    return json{{"ts_ms", s.ts_ms()}, {"cpu_pct", s.cpu_pct()}, {"mem_mb", s.mem_mb()},
                {"http_req_rate", s.http_req_rate()}, {"rate_limit_hits", s.rate_limit_hits()},
                {"db_write_ms", s.db_write_ms()}, {"thread_count", s.thread_count()}};
}

json j_PoolReward(const PoolReward& s) {
    return json{{"ts_ms", s.ts_ms()}, {"condition_id", s.condition_id()}, {"question", s.question()},
                {"est_reward", s.est_reward()}, {"realized_reward", s.realized_reward()},
                {"share", s.share()}};
}

// ---- 每 topic 的 reader: 收到样本 → to_json → 打一行 {"topic","data"} ----
template <typename T>
using ToJson = json (*)(const T&);

template <typename T>
class TopicReader final : public efd::DataReaderListener {
public:
    TopicReader(std::string name, ToJson<T> fn) : name_(std::move(name)), fn_(fn) {}

    void on_data_available(efd::DataReader* reader) override {
        T sample;
        efd::SampleInfo info;
        while (reader->take_next_sample(&sample, &info) == efd::RETCODE_OK) {
            if (!info.valid_data) continue;
            json frame;
            frame["topic"] = name_;
            frame["data"] = fn_(sample);
            const std::string line = frame.dump();  // 紧凑单行
            std::lock_guard<std::mutex> lk(g_out_mu);
            std::cout << line << '\n';
            std::cout.flush();
        }
    }

private:
    std::string name_;
    ToJson<T> fn_;
};

// 注册一个 topic 的 type + reader; listener 存入 keep 保活。
template <typename T, typename PubSubT>
void add_reader(efd::DomainParticipant* dp, efd::Subscriber* sub,
                std::vector<std::unique_ptr<efd::DataReaderListener>>& keep,
                const char* name, ToJson<T> fn) {
    efd::TypeSupport ts(new PubSubT());
    ts.register_type(dp);
    efd::Topic* tp = dp->create_topic(name, ts.get_type_name(), efd::TOPIC_QOS_DEFAULT);
    if (tp == nullptr) {
        std::fprintf(stderr, "tap: create_topic failed: %s\n", name);
        return;
    }
    efd::DataReaderQos rq = efd::DATAREADER_QOS_DEFAULT;
    rq.history().kind = efd::KEEP_LAST_HISTORY_QOS;
    rq.history().depth = 16;
    rq.reliability().kind = efd::BEST_EFFORT_RELIABILITY_QOS;  // 尽力而为, 匹配 RELIABLE/BEST_EFFORT 发布者
    auto listener = std::make_unique<TopicReader<T>>(name, fn);
    efd::DataReader* dr = sub->create_datareader(tp, rq, listener.get());
    if (dr == nullptr) {
        std::fprintf(stderr, "tap: create_datareader failed: %s\n", name);
        return;
    }
    keep.push_back(std::move(listener));
}

}  // namespace

int main() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    efd::DomainParticipant* dp =
        efd::DomainParticipantFactory::get_instance()->create_participant(0, efd::PARTICIPANT_QOS_DEFAULT);
    if (dp == nullptr) {
        std::fprintf(stderr, "tap: create_participant failed\n");
        return 1;
    }
    efd::Subscriber* sub = dp->create_subscriber(efd::SUBSCRIBER_QOS_DEFAULT);
    if (sub == nullptr) {
        std::fprintf(stderr, "tap: create_subscriber failed\n");
        return 1;
    }

    std::vector<std::unique_ptr<efd::DataReaderListener>> keep;
    keep.reserve(21);
    add_reader<Heartbeat, HeartbeatPubSubType>(dp, sub, keep, topic::kHeartbeat, &j_Heartbeat);
    add_reader<QuoteDecision, QuoteDecisionPubSubType>(dp, sub, keep, topic::kQuoteDecision, &j_QuoteDecision);
    add_reader<PoolEval, PoolEvalPubSubType>(dp, sub, keep, topic::kPoolEval, &j_PoolEval);
    add_reader<FillContext, FillContextPubSubType>(dp, sub, keep, topic::kFillContext, &j_FillContext);
    add_reader<OrderBookL2, OrderBookL2PubSubType>(dp, sub, keep, topic::kOrderBookL2, &j_OrderBookL2);
    add_reader<RawFeed, RawFeedPubSubType>(dp, sub, keep, topic::kRawFeed, &j_RawFeed);
    add_reader<Position, PositionPubSubType>(dp, sub, keep, topic::kPosition, &j_Position);
    add_reader<EquitySnapshot, EquitySnapshotPubSubType>(dp, sub, keep, topic::kEquitySnapshot, &j_EquitySnapshot);
    add_reader<KillEvent, KillEventPubSubType>(dp, sub, keep, topic::kKillEvent, &j_KillEvent);
    add_reader<RewardSnapshot, RewardSnapshotPubSubType>(dp, sub, keep, topic::kRewardSnapshot, &j_RewardSnapshot);
    add_reader<LogEvent, LogEventPubSubType>(dp, sub, keep, topic::kLogEvent, &j_LogEvent);
    add_reader<OrderEvent, OrderEventPubSubType>(dp, sub, keep, topic::kOrderEvent, &j_OrderEvent);
    add_reader<TradePrint, TradePrintPubSubType>(dp, sub, keep, topic::kTradePrint, &j_TradePrint);
    add_reader<FeedHealth, FeedHealthPubSubType>(dp, sub, keep, topic::kFeedHealth, &j_FeedHealth);
    add_reader<LatencyProfile, LatencyProfilePubSubType>(dp, sub, keep, topic::kLatencyProfile, &j_LatencyProfile);
    add_reader<ChainState, ChainStatePubSubType>(dp, sub, keep, topic::kChainState, &j_ChainState);
    add_reader<RiskState, RiskStatePubSubType>(dp, sub, keep, topic::kRiskState, &j_RiskState);
    add_reader<DiscoveryScan, DiscoveryScanPubSubType>(dp, sub, keep, topic::kDiscoveryScan, &j_DiscoveryScan);
    add_reader<ConfigSnapshot, ConfigSnapshotPubSubType>(dp, sub, keep, topic::kConfigSnapshot, &j_ConfigSnapshot);
    add_reader<SystemHealth, SystemHealthPubSubType>(dp, sub, keep, topic::kSystemHealth, &j_SystemHealth);
    add_reader<PoolReward, PoolRewardPubSubType>(dp, sub, keep, topic::kPoolReward, &j_PoolReward);

    std::fprintf(stderr, "tap: subscribed to %zu topics on domain 0; streaming JSON to stdout...\n",
                 keep.size());

    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::fprintf(stderr, "tap: shutting down\n");
    dp->delete_contained_entities();
    efd::DomainParticipantFactory::get_instance()->delete_participant(dp);
    return 0;
}
