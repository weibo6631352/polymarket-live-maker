// apps/scalp_trial_main.cpp — tiny, HARD-CAPPED real-money crypto-scalp TRIAL.
//
// Purpose: answer the ONE question dry analysis can't — does taking PM "Bitcoin/Ethereum
// Up or Down" stale quotes (when Binance/Kraken spot has already moved) actually PROFIT
// after adverse selection, or do we get picked off like the 42 losing bots?
//
// SAFETY (the whole point — bounds the user's downside):
//   * SERIAL: at most ONE open position at a time. No concurrent runaway possible.
//   * HARD NET-LOSS KILL: cumulative realized loss reaches LM_SCALP_MAXLOSS ($30 default)
//     -> cancel + stop permanently, never trade again this run.
//   * PER-ORDER size cap (LM_SCALP_ORDER, $2 default), MAX trades (LM_SCALP_MAXTRADES).
//   * KILL FILE: touch the kill-file -> immediate stop (manual one-touch kill switch).
//   * DRY by default: places NO real order unless PM_TRADER_LIVE=1 AND LM_SCALP_ARM=1
//     (double-gate). In dry mode every "order" is logged, P&L simulated -> proves the
//     safety logic before a cent is risked.
//
//   PM_TRADER_LIVE=1 LM_SCALP_ARM=1 ./scalp-trial      (REAL, hard-capped)
//   ./scalp-trial                                       (DRY rehearsal, no money)
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>

#include "pmm/app/dotenv.hpp"
#include "pmm/clob_submitter.hpp"

namespace {

double env_d(const char* k, double def) {
    const char* v = std::getenv(k);
    return v != nullptr ? std::atof(v) : def;
}
int env_i(const char* k, int def) {
    const char* v = std::getenv(k);
    return v != nullptr ? std::atoi(v) : def;
}
bool file_exists(const std::string& p) {
    struct stat st {};
    return ::stat(p.c_str(), &st) == 0;
}

// ---- the hard safety state machine (the part that MUST be bug-free) ----
struct Safety {
    double max_loss;     // stop when cum_realized <= -max_loss
    double order_usd;    // per-order notional cap
    int max_trades;      // stop after this many trades
    std::string killfile;
    // live state
    double cum_realized{0.0};  // realized P&L (USD), updated only on resolved positions
    int trades{0};
    bool open{false};   // SERIAL guard: a position is currently open
    bool stopped{false};
    std::string stop_reason;

    [[nodiscard]] bool can_open() {
        if (stopped) return false;
        if (open) return false;                              // serial: one at a time
        if (cum_realized <= -max_loss) { stop("MAXLOSS $" + std::to_string(max_loss)); return false; }
        if (trades >= max_trades) { stop("MAXTRADES"); return false; }
        if (file_exists(killfile)) { stop("KILLFILE"); return false; }
        return true;
    }
    void on_open() { open = true; ++trades; }
    void on_close(double realized) {  // realized P&L of the just-closed position
        cum_realized += realized;
        open = false;
        if (cum_realized <= -max_loss) stop("MAXLOSS $" + std::to_string(max_loss));
    }
    void stop(const std::string& why) {
        if (!stopped) { stopped = true; stop_reason = why; }
    }
};

}  // namespace

int main() {
    Safety sf{
        /*max_loss=*/env_d("LM_SCALP_MAXLOSS", 30.0),
        /*order_usd=*/env_d("LM_SCALP_ORDER", 2.0),
        /*max_trades=*/env_i("LM_SCALP_MAXTRADES", 40),
        /*killfile=*/std::string(std::getenv("LM_SCALP_KILLFILE") != nullptr
                                     ? std::getenv("LM_SCALP_KILLFILE")
                                     : "state/scalp.kill"),
    };

    const bool want_live = std::getenv("PM_TRADER_LIVE") != nullptr &&
                           std::string(std::getenv("PM_TRADER_LIVE")) == "1";
    const bool armed = std::getenv("LM_SCALP_ARM") != nullptr &&
                       std::string(std::getenv("LM_SCALP_ARM")) == "1";
    const bool LIVE = want_live && armed;  // double-gate

    std::printf("=== scalp-trial ===\n");
    std::printf("mode      : %s\n", LIVE ? "LIVE — REAL MONEY (double-gated)" : "DRY rehearsal (no orders)");
    std::printf("max_loss  : $%.2f (hard kill)\n", sf.max_loss);
    std::printf("order_usd : $%.2f/trade   max_trades: %d\n", sf.order_usd, sf.max_trades);
    std::printf("killfile  : %s (touch to stop)\n", sf.killfile.c_str());

    pmm::app::LoadDotEnv(".env", /*live_mode=*/LIVE);
    pmm::clob::ClobSubmitter sub;
    if (LIVE && !sub.ready()) {
        std::fprintf(stderr, "LIVE requested but ClobSubmitter NOT ready (key/creds). Aborting.\n");
        return 1;
    }
    if (LIVE) std::printf("signer    : %s\n", sub.signer_address().c_str());

    // TODO(next, verified in dry-test against live data):
    //   1. discover active BTC/ETH "Up or Down" micro-markets (gamma-api) + window open/close + token ids
    //   2. Kraken WS (WsConnection) -> latest BTC/ETH price; window-open price snapshot
    //   3. fair P(Up) vs PM stale quote -> mispricing signal
    //   4. when sf.can_open(): place ONE order_usd order on the cheap side (ClobSubmitter / dry-log)
    //   5. hold to resolution -> realized P&L -> sf.on_close(realized)
    //   6. loop until sf.stopped
    std::printf("\n[skeleton] safety state machine wired; data/strategy legs added + dry-verified next.\n");
    std::printf("STOP if set: %s\n", sf.stopped ? sf.stop_reason.c_str() : "(running)");
    return 0;
}
