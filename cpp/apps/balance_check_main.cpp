// apps/balance_check_main.cpp — READ-ONLY credential + collateral verification.
//
// Loads .env creds, prints the signer EOA derived from the key, and queries the
// CLOB /balance-allowance (asset_type=COLLATERAL) exactly ONCE. It NEVER submits
// an order — there is no PLACE in this binary. Use it to confirm the configured
// private key actually controls POLYMARKET_FUNDER and that the funded USDC is
// visible to the bot, without placing any trade.
//
//   PM_TRADER_LIVE=1 ./balance-check        (run from a dir containing .env)
#include <cstdio>
#include <optional>

#include "pmm/app/dotenv.hpp"
#include "pmm/clob_submitter.hpp"

int main() {
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
    return 0;
}
