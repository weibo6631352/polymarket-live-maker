// apps/flatten_main.cpp — one-off: 市价平掉一个持仓 (SELL 多头 / BUY 回补空头) 换回 USDC。
//
// 用途: 当 bot 因 fill 检测漏单留下未跟踪库存时, 人工把它平回 USDC。复用 ClobSubmitter::flatten
// (取 marketable 价 + 市价单成交)。REAL ORDER —— 会真实成交。需 PM_TRADER_LIVE=1, 从含 .env 的目录运行。
//   PM_TRADER_LIVE=1 ./flatten <token_id> <size> [side=SELL]
#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>

#include "pmm/app/dotenv.hpp"
#include "pmm/clob_submitter.hpp"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: PM_TRADER_LIVE=1 flatten <token_id> <size> [side=SELL]\n");
        return 2;
    }
    const std::string tok = argv[1];
    const double size = std::stod(argv[2]);
    const std::string side = argc > 3 ? argv[3] : "SELL";

    pmm::app::LoadDotEnv(".env", /*live_mode=*/true);
    pmm::clob::ClobSubmitter sub;
    if (!sub.ready()) {
        std::fprintf(stderr, "ClobSubmitter NOT ready — set PM_TRADER_LIVE=1 and check key/creds.\n");
        return 1;
    }
    std::printf("signer: %s\n", sub.signer_address().c_str());
    std::printf("=== FLATTEN %s %g on %s (marketable, will fill) ===\n", side.c_str(), size, tok.c_str());
    const nlohmann::json res =
        sub({{"action", "FLATTEN"}, {"token_id", tok}, {"side", side}, {"size", size}});
    std::printf("%s\n", res.dump(2).c_str());
    return 0;
}
