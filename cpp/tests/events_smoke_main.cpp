// apps/events_smoke_main.cpp — EventLog 自检: 写事件 → flush → 读回当天 jsonl 校验 JSON 完整性。
#include "pmm/events.hpp"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}
std::string today_utc() {
    const std::time_t tt = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    char d[16];
    std::snprintf(d, sizeof(d), "%04d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return d;
}
}  // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "pmm_events_smoke";
    std::error_code ec;
    fs::remove_all(dir, ec);

    {
        pmm::EventLog log(dir, /*retention_days=*/30);
        log.write("discovery", {{"safe_pools", 7}, {"scanned", 120}});
        log.write("poll", {{"share", 0.013}, {"mid", 0.52}, {"reward", 1.25}});
        log.write("decision", {{"action", "place"}, {"pool", "0xabc"}});
        log.close();  // flush 已入队事件
    }  // 析构再 drain 一次

    const fs::path f = dir / ("events-" + today_utc() + ".jsonl");
    Check(fs::exists(f), "today's events file exists");

    std::ifstream in(f);
    int lines = 0;
    bool all_valid = true;
    bool saw_poll_mid = false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        ++lines;
        try {
            const auto j = nlohmann::json::parse(line);
            if (!j.contains("ts") || !j.contains("kind")) all_valid = false;
            if (j.value("kind", "") == "poll" && j.contains("mid") &&
                std::abs(j["mid"].get<double>() - 0.52) < 1e-9) {
                saw_poll_mid = true;
            }
        } catch (...) {
            all_valid = false;
        }
    }
    Check(lines == 3, "wrote 3 event lines");
    Check(all_valid, "every line is valid JSON with ts+kind");
    Check(saw_poll_mid, "poll event carries mid=0.52 field");

    std::printf(g_fail ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
