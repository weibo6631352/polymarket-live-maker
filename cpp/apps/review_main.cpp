// apps/review_main.cpp — review-report 入口: 打印 估计 vs 链上实际 奖励对账报告 (port of review.main)。
//
// 运行: ./review-report [days]   (读 .env: LM_STATE_DIR, POLYMARKET_FUNDER; days 默认 10)
#include <cstdio>
#include <cstdlib>
#include <string>

#include "pmm/app/dotenv.hpp"
#include "pmm/review.hpp"

int main(int argc, char** argv) {
    pmm::app::LoadDotEnv(".env", /*live_mode=*/false);
    const char* sd = std::getenv("LM_STATE_DIR");
    const std::string state_dir = sd != nullptr ? sd : "state";
    const char* w = std::getenv("POLYMARKET_FUNDER");
    const std::string wallet = w != nullptr ? w : "";
    const int days = argc > 1 ? std::atoi(argv[1]) : 10;
    if (wallet.empty()) {
        std::printf("(no POLYMARKET_FUNDER set — showing estimate only, no actual reconciliation)\n");
    }
    const std::string report = pmm::review::format_report(pmm::review::run_review(state_dir, wallet, days));
    std::printf("%s\n", report.c_str());
    return 0;
}
