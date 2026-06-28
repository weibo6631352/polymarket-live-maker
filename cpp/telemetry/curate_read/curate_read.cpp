// telemetry/curate_read/curate_read.cpp — 订阅 PoolEval ~Ns, 每 cond 留最新, 打印候选表 (host 接收侧)。
//
// 外部 curator (LLM session) 用它经 DDS 接收 bot 发布的实时候选池 + 选池经济学字段, 据此筛选 →
// 再用 curate_push 把白名单推回。闭环的"接收"半边。仅 WITH_TELEMETRY 编译。
//
// 用法:  curate_read [秒数=4]
// 输出每行一池, 按 est_reward 降序; curator 可据 reward_rate_per_day / competitiveness 算奖励份额。
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>

#include "pmm/telemetry/publisher.hpp"
#include "telemetryPubSubTypes.hpp"

namespace efd = eprosima::fastdds::dds;
using namespace pmm::telemetry;

namespace {
std::mutex g_mu;
std::map<std::string, PoolEval> g_latest;  // cond -> 最新样本

class PoolEvalReader final : public efd::DataReaderListener {
public:
    void on_data_available(efd::DataReader* reader) override {
        PoolEval s;
        efd::SampleInfo info;
        while (reader->take_next_sample(&s, &info) == efd::RETCODE_OK) {
            if (!info.valid_data) continue;
            std::lock_guard<std::mutex> lk(g_mu);
            g_latest[s.condition_id()] = s;
        }
    }
};
}  // namespace

int main(int argc, char** argv) {
    // 默认 18s: 略大于 bot 的 PoolEval 周期重发间隔 (15s), 保证无参一跑就拿到全量当前候选集。
    const double secs = argc > 1 ? std::atof(argv[1]) : 18.0;

    efd::DomainParticipant* dp = efd::DomainParticipantFactory::get_instance()->create_participant(
        0, efd::PARTICIPANT_QOS_DEFAULT);
    if (dp == nullptr) {
        std::fprintf(stderr, "curate_read: create_participant failed\n");
        return 1;
    }
    efd::Subscriber* sub = dp->create_subscriber(efd::SUBSCRIBER_QOS_DEFAULT);
    efd::TypeSupport ts(new PoolEvalPubSubType());
    ts.register_type(dp);
    efd::Topic* tp = dp->create_topic(topic::kPoolEval, ts.get_type_name(), efd::TOPIC_QOS_DEFAULT);
    if (tp == nullptr) {
        std::fprintf(stderr, "curate_read: create_topic failed\n");
        return 1;
    }
    efd::DataReaderQos rq = efd::DATAREADER_QOS_DEFAULT;
    // 与 bot 的 keyed PoolEval writer 匹配: RELIABLE + TRANSIENT_LOCAL + KEEP_LAST(1)/instance →
    // 一连上就立即收到"每池最新 PoolEval"(全量当前候选集), 不必赶在 scan 突发窗口内。
    rq.history().kind = efd::KEEP_LAST_HISTORY_QOS;
    rq.history().depth = 1;
    rq.reliability().kind = efd::RELIABLE_RELIABILITY_QOS;
    rq.durability().kind = efd::TRANSIENT_LOCAL_DURABILITY_QOS;
    rq.resource_limits().max_samples_per_instance = 1;
    rq.resource_limits().max_instances = 16384;
    rq.resource_limits().max_samples = 16384;
    PoolEvalReader listener;
    efd::DataReader* dr = sub->create_datareader(tp, rq, &listener);
    if (dr == nullptr) {
        std::fprintf(stderr, "curate_read: create_datareader failed\n");
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::duration<double>(secs));

    std::vector<PoolEval> v;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (auto& [k, s] : g_latest) v.push_back(s);
    }
    std::sort(v.begin(), v.end(),
              [](const PoolEval& a, const PoolEval& b) { return a.est_reward() > b.est_reward(); });

    std::printf(
        "# %zu candidate pools (PoolEval, sorted by est_reward). "
        "cols: cond | mid | comp | vol24$ | rawrate$/d | est_rew | net/d | days | jump | empty | "
        "question\n",
        v.size());
    for (auto& s : v) {
        std::printf("%s | %.3f | %.1f | %.0f | %.1f | %.2f | %.2f | %.1f | %s | %d | %s\n",
                    s.condition_id().c_str(), s.mid(), s.competitiveness(), s.volume_24hr(),
                    s.reward_rate_per_day(), s.est_reward(), s.net_per_day(), s.days_to_resolution(),
                    s.jump_verdict().c_str(), s.empty_band() ? 1 : 0, s.question().c_str());
    }

    dp->delete_contained_entities();
    efd::DomainParticipantFactory::get_instance()->delete_participant(dp);
    return 0;
}
