// pmm/events.hpp — append-only 事件日志 (port of pm_trader/events.py)
//
// 每行一个 JSON 对象 {ts, kind, ...fields}, 写到日期分文件 events-YYYYMMDD.jsonl, 滚动 N 天保留。
// 采纳 sports-trader-cpp 的 BackgroundWriter: 入队纳秒级、独立线程批量落盘 → 写日志永不阻塞 1s 决策环
//   (比 Python 每行 flush 更优; 代价是崩溃最多丢 ~1s 事件, journal 容忍丢失)。Best-effort: write 永不抛。
#pragma once

#include <filesystem>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "pmm/infra/background_writer.hpp"

namespace pmm {

class EventLog {
public:
    explicit EventLog(std::filesystem::path directory, int retention_days = 30);

    // 追加一个事件 {ts, kind, ...fields}。best-effort, 永不抛入热环。
    void write(const std::string& kind, const nlohmann::json& fields = nlohmann::json::object());

    // 删除超出保留窗口的 events-*.jsonl (YYYYMMDD 字典序即时序)。
    void prune();

    // 关停前 flush 已入队的事件。
    void close();

private:
    [[nodiscard]] std::filesystem::path path_for(const std::string& day) const;

    std::filesystem::path dir_;
    int retention_days_;
    std::mutex mu_;
    std::string cur_day_;
    pmm::infra::BackgroundWriter writer_;
};

}  // namespace pmm
