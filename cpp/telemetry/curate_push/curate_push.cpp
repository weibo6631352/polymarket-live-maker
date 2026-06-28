// telemetry/curate_push/curate_push.cpp — 推一条 CuratorCommand 白名单到 DDS (host→bot 控制)。
//
// 这是外部 curator (LLM session) 选完池后, 把选中池集经 DDS 实时推给运行中的 bot 的工具。bot 端
// DdsPublisher 的 CuratorCommand reader 收到 → 热更 dyn_whitelist_, 不重启。仅 WITH_TELEMETRY 编译。
//
// 用法:  curate_push "<cond1>,<cond2>,..." "<note 理由摘要>"
//        curate_push ""                       (空串 = 清空白名单)
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <thread>

#include <fastdds/dds/core/status/PublicationMatchedStatus.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>

#include "pmm/telemetry/publisher.hpp"  // topic:: 常量
#include "telemetryPubSubTypes.hpp"     // 生成的 CuratorCommand + PubSubType

namespace efd = eprosima::fastdds::dds;
using namespace pmm::telemetry;

int main(int argc, char** argv) {
    const std::string whitelist = argc > 1 ? std::string(argv[1]) : std::string();
    const std::string note = argc > 2 ? std::string(argv[2]) : std::string();

    efd::DomainParticipant* dp = efd::DomainParticipantFactory::get_instance()->create_participant(
        0, efd::PARTICIPANT_QOS_DEFAULT);
    if (dp == nullptr) {
        std::fprintf(stderr, "curate_push: create_participant failed\n");
        return 1;
    }
    efd::Publisher* pub = dp->create_publisher(efd::PUBLISHER_QOS_DEFAULT);
    efd::TypeSupport ts(new CuratorCommandPubSubType());
    ts.register_type(dp);
    efd::Topic* tp =
        dp->create_topic(topic::kCuratorCommand, ts.get_type_name(), efd::TOPIC_QOS_DEFAULT);
    if (tp == nullptr) {
        std::fprintf(stderr, "curate_push: create_topic failed\n");
        return 1;
    }
    efd::DataWriterQos wq = efd::DATAWRITER_QOS_DEFAULT;
    // RELIABLE + TRANSIENT_LOCAL: 与 bot reader 匹配, 即使发现略晚也把这条白名单送达 (绝不丢)。
    wq.reliability().kind = efd::RELIABLE_RELIABILITY_QOS;
    wq.durability().kind = efd::TRANSIENT_LOCAL_DURABILITY_QOS;
    wq.history().kind = efd::KEEP_LAST_HISTORY_QOS;
    wq.history().depth = 4;
    efd::DataWriter* dw = pub->create_datawriter(tp, wq);
    if (dw == nullptr) {
        std::fprintf(stderr, "curate_push: create_datawriter failed\n");
        return 1;
    }

    // 等到至少有一个匹配的 reader (运行中的 bot) 再写, 最多 ~3s。
    efd::PublicationMatchedStatus ms;
    bool matched = false;
    for (int i = 0; i < 30; ++i) {
        dw->get_publication_matched_status(ms);
        if (ms.current_count > 0) {
            matched = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    CuratorCommand cmd;
    cmd.ts_ms(static_cast<std::int64_t>(std::time(nullptr)) * 1000);
    cmd.whitelist(whitelist);
    cmd.note(note);
    dw->write(&cmd);
    // RELIABLE 确认送达 (最多 3s); TRANSIENT_LOCAL 兜底晚到的 reader。
    const bool acked = dw->wait_for_acknowledgments(efd::Duration_t{3, 0}) == efd::RETCODE_OK;

    const std::size_t n =
        whitelist.empty()
            ? 0
            : static_cast<std::size_t>(std::count(whitelist.begin(), whitelist.end(), ',') + 1);
    const bool ok = matched && acked;
    std::fprintf(stderr, "curate_push: %s %zu pools (matched=%d acked=%d note=%s)\n",
                 ok ? "delivered" : "NO-BOT-MATCHED", n, matched ? 1 : 0, acked ? 1 : 0, note.c_str());

    dp->delete_contained_entities();
    efd::DomainParticipantFactory::get_instance()->delete_participant(dp);
    return ok ? 0 : 1;
}
