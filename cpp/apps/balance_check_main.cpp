// apps/balance_check_main.cpp — READ-ONLY credential + collateral verification.
//
// Loads .env creds, prints the signer EOA derived from the key, and queries the
// CLOB /balance-allowance (asset_type=COLLATERAL) exactly ONCE. It NEVER submits
// an order — there is no PLACE in this binary. Use it to confirm the configured
// private key actually controls POLYMARKET_FUNDER and that the funded USDC is
// visible to the bot, without placing any trade.
//
//   PM_TRADER_LIVE=1 ./balance-check          (signer + collateral; zero orders)
//   PM_TRADER_LIVE=1 ./balance-check fills     (also dump poll_fills() — verify fill detection; zero orders)
#include <cstdio>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "pmm/app/dotenv.hpp"
#include "pmm/clob_submitter.hpp"

int main(int argc, char** argv) {
    pmm::app::LoadDotEnv(".env", /*live_mode=*/true);  // creds from cwd .env
    pmm::clob::ClobSubmitter sub;
    if (!sub.ready()) {
        std::fprintf(stderr,
                     "ClobSubmitter NOT ready — need PM_TRADER_LIVE=1 and a valid key/derived creds.\n");
        return 1;
    }
    std::printf("signer (EOA derived from key): %s\n", sub.signer_address().c_str());
    const std::optional<double> bal = sub.usdc_balance();
    if (bal) {
        std::printf("CLOB collateral visible to bot (USDC): %.6f\n", *bal);
    } else {
        std::printf("CLOB collateral query FAILED/empty (auth failure or key/funder mismatch?)\n");
        return 3;
    }
    if (argc > 1 && std::string(argv[1]) == "fills") {
        // 只读: 按 funder maker_address 查最近成交 — 验证 fill 检测 (不下单)。
        const std::vector<nlohmann::json> fills = sub.poll_fills();
        std::printf("poll_fills() returned %zu trade(s):\n", fills.size());
        for (const auto& f : fills) std::printf("  %s\n", f.dump().c_str());
    }
    return 0;
}
