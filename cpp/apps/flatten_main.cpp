// apps/flatten_main.cpp — one-off: 市价平掉一个持仓 (SELL 多头 / BUY 回补空头) 换回 USDC。
//
// 用途: 当 bot 因 fill 检测漏单留下未跟踪库存时, 人工把它平回 USDC。REAL ORDER —— 会真实成交。
// 需 PM_TRADER_LIVE=1, 从含 .env 的目录运行。
//   PM_TRADER_LIVE=1 ./flatten <token_id> <size> [side=SELL] [price]
// 不给 price → 走 ClobSubmitter FLATTEN (marketable FOK; 薄盘口/抢单时可能 killed)。
// 给 price → 直接挂一张激进限价单 (GTC, 跨过盘口立即成交, 部分成交也不 kill); 平薄盘口更稳。
#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>

#include "pmm/app/dotenv.hpp"
#include "pmm/clob_submitter.hpp"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: PM_TRADER_LIVE=1 flatten <token_id> <size> [side=SELL] [price]\n");
        return 2;
    }
    const std::string tok = argv[1];
    const double size = std::stod(argv[2]);
    const std::string side = argc > 3 ? argv[3] : "SELL";
    const bool has_price = argc > 4;
    const double price = has_price ? std::stod(argv[4]) : 0.0;

    pmm::app::LoadDotEnv(".env", /*live_mode=*/true);
    pmm::clob::ClobSubmitter sub;
    if (!sub.ready()) {
        std::fprintf(stderr, "ClobSubmitter NOT ready — set PM_TRADER_LIVE=1 and check key/creds.\n");
        return 1;
    }
    std::printf("signer: %s\n", sub.signer_address().c_str());
    nlohmann::json res;
    if (has_price) {
        std::printf("=== PLACE %s %g @ %g on %s (marketable limit, crosses to fill) ===\n", side.c_str(),
                    size, price, tok.c_str());
        res = sub({{"action", "PLACE"}, {"token_id", tok}, {"side", side}, {"price", price}, {"size", size}});
    } else {
        std::printf("=== FLATTEN %s %g on %s (marketable FOK) ===\n", side.c_str(), size, tok.c_str());
        res = sub({{"action", "FLATTEN"}, {"token_id", tok}, {"side", side}, {"size", size}});
    }
    std::printf("%s\n", res.dump(2).c_str());
    return 0;
}
