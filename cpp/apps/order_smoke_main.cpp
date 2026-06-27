// apps/order_smoke_main.cpp — controlled LIVE signing smoke.
//
// Places ONE limit BUY far below the mid (cannot fill), then cancels it — to verify
// Polymarket ACCEPTS our from-scratch C++ V2-signed orders (EIP-712 + secp256k1) on
// the live CLOB. This is the one thing the parity tests can't prove offline.
//
// REAL ORDER (tiny, far from mid). Run from a dir with .env; needs PM_TRADER_LIVE=1.
//   PM_TRADER_LIVE=1 ./order-smoke <token_id> [price=0.05] [size=5]
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "pmm/app/dotenv.hpp"
#include "pmm/clob_submitter.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: PM_TRADER_LIVE=1 order-smoke <token_id> [price=0.05] [size=5]\n");
        return 2;
    }
    const std::string tok = argv[1];
    const double price = argc > 2 ? std::stod(argv[2]) : 0.05;
    const double size = argc > 3 ? std::stod(argv[3]) : 5.0;

    pmm::app::LoadDotEnv(".env", /*live_mode=*/true);  // creds from cwd .env (env wins → PM_TRADER_LIVE=1)
    pmm::clob::ClobSubmitter sub;
    if (!sub.ready()) {
        std::fprintf(stderr, "ClobSubmitter NOT ready — set PM_TRADER_LIVE=1 and check key/derived creds.\n");
        return 1;
    }
    std::printf("signer: %s\n", sub.signer_address().c_str());

    std::printf("=== PLACE BUY size=%g @ %g on %s (far below mid -> must NOT fill) ===\n",
                size, price, tok.c_str());
    const nlohmann::json placed =
        sub({{"action", "PLACE"}, {"token_id", tok}, {"side", "BUY"}, {"price", price}, {"size", size}});
    std::printf("%s\n", placed.dump(2).c_str());

    std::this_thread::sleep_for(std::chrono::seconds(3));

    std::printf("=== CANCEL_ALL on %s ===\n", tok.c_str());
    const nlohmann::json cancelled = sub({{"action", "CANCEL_ALL"}, {"token_id", tok}});
    std::printf("%s\n", cancelled.dump(2).c_str());
    return 0;
}
