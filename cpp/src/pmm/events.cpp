// src/pmm/events.cpp — EventLog 实现 (port of pm_trader/events.py)
#include "pmm/events.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

namespace pmm {

namespace {

// UTC 当前时刻: 返回 (iso8601 含微秒, "YYYYMMDD")。
std::pair<std::string, std::string> utc_now() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto secs = time_point_cast<seconds>(now);
    const auto us = duration_cast<microseconds>(now - secs).count();
    const std::time_t tt = system_clock::to_time_t(secs);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    char iso[40];
    std::snprintf(iso, sizeof(iso), "%04d-%02d-%02dT%02d:%02d:%02d.%06ld+00:00", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<long>(us));
    char day[16];
    std::snprintf(day, sizeof(day), "%04d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return {std::string(iso), std::string(day)};
}

}  // namespace

EventLog::EventLog(std::filesystem::path directory, int retention_days)
    : dir_(std::move(directory)), retention_days_(std::max(1, retention_days)) {
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    prune();
}

std::filesystem::path EventLog::path_for(const std::string& day) const {
    return dir_ / ("events-" + day + ".jsonl");
}

void EventLog::write(const std::string& kind, const nlohmann::json& fields) {
    try {
        const auto [iso, day] = utc_now();
        // {ts, kind, **fields} — fields 覆盖 ts/kind (与 Python **fields 语义一致)。
        nlohmann::json obj;
        obj["ts"] = iso;
        obj["kind"] = kind;
        if (fields.is_object()) {
            for (auto it = fields.begin(); it != fields.end(); ++it) obj[it.key()] = it.value();
        }
        std::string line = obj.dump();
        line.push_back('\n');
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (day != cur_day_) {  // 首次写 / 跨日
                cur_day_ = day;
                prune();  // 跨日时滚掉旧日文件
            }
            writer_.AppendLine(path_for(day).string(), std::move(line));
        }
    } catch (...) {
        // 丢一行日志绝不能打断交易。
    }
}

void EventLog::prune() {
    try {
        // cutoff = (now - retention_days) 的 YYYYMMDD。
        using namespace std::chrono;
        const auto cutoff_tp = system_clock::now() - hours(24 * retention_days_);
        const std::time_t tt = system_clock::to_time_t(cutoff_tp);
        std::tm tm{};
        gmtime_r(&tt, &tm);
        char cutoff[16];
        std::snprintf(cutoff, sizeof(cutoff), "%04d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                      tm.tm_mday);
        const std::string cutoff_s(cutoff);

        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
            const std::string name = entry.path().filename().string();
            // events-YYYYMMDD.jsonl
            if (name.rfind("events-", 0) != 0) continue;
            const auto dot = name.find(".jsonl");
            if (dot == std::string::npos) continue;
            const std::string day = name.substr(7, dot - 7);
            const bool all_digit =
                day.size() == 8 && std::all_of(day.begin(), day.end(), [](char c) {
                    return c >= '0' && c <= '9';
                });
            if (all_digit && day < cutoff_s) {
                std::filesystem::remove(entry.path(), ec);
            }
        }
    } catch (...) {
    }
}

void EventLog::close() {
    writer_.Flush();        // 先把队列里的事件 drain 进 per-file 缓冲
    writer_.FlushToDisk();  // 再强制缓冲落盘 (对齐 Python: close() 返回后事件必在磁盘)
}

}  // namespace pmm
