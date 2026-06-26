// apps/clob_amounts_parity_main.cpp — ClobSubmitter 金额计算与 py-clob-client 对齐 (golden 见 gen_amounts.py)。
#include "pmm/clob_submitter.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

namespace C = pmm::clob;

namespace {
int g_fail = 0;
void Eq(std::uint64_t got, std::uint64_t want, const std::string& what) {
    const bool ok = got == want;
    std::printf("[%s] %-28s got=%llu want=%llu\n", ok ? "PASS" : "FAIL", what.c_str(),
                static_cast<unsigned long long>(got), static_cast<unsigned long long>(want));
    if (!ok) ++g_fail;
}
}  // namespace

int main() {
    struct Lim {
        bool buy;
        double size;
        double price;
        const char* tick;
        int side;
        std::uint64_t mk;
        std::uint64_t tk;
        const char* name;
    };
    const Lim lim[] = {
        {true, 100.0, 0.52, "0.01", 0, 52000000ULL, 100000000ULL, "lim buy 100@.52"},
        {true, 12.34, 0.57, "0.01", 0, 7033800ULL, 12340000ULL, "lim buy 12.34@.57"},
        {false, 12.34, 0.57, "0.01", 1, 12340000ULL, 7033800ULL, "lim sell 12.34@.57"},
        {true, 33.33, 0.123, "0.001", 0, 4099590ULL, 33330000ULL, "lim buy 33.33@.123"},
        {false, 250.0, 0.045, "0.001", 1, 250000000ULL, 11250000ULL, "lim sell 250@.045"},
        {true, 7.5, 0.9, "0.1", 0, 6750000ULL, 7500000ULL, "lim buy 7.5@.9"},
    };
    for (const auto& t : lim) {
        const C::OrderAmounts a = C::get_order_amounts(t.buy, t.size, t.price, C::round_config(t.tick));
        Eq(static_cast<std::uint64_t>(a.side), static_cast<std::uint64_t>(t.side), std::string(t.name) + " side");
        Eq(a.maker_amount, t.mk, std::string(t.name) + " maker");
        Eq(a.taker_amount, t.tk, std::string(t.name) + " taker");
    }

    struct Mkt {
        bool buy;
        double amount;
        double price;
        const char* tick;
        std::uint64_t mk;
        std::uint64_t tk;
        const char* name;
    };
    const Mkt mkt[] = {
        {true, 100.0, 0.50, "0.01", 100000000ULL, 200000000ULL, "mkt buy 100usdc@.50"},
        {false, 50.0, 0.50, "0.01", 50000000ULL, 25000000ULL, "mkt sell 50sh@.50"},
        {true, 99.0, 0.123, "0.001", 99000000ULL, 804878040ULL, "mkt buy 99usdc@.123"},
    };
    for (const auto& t : mkt) {
        const C::OrderAmounts a = C::get_market_order_amounts(t.buy, t.amount, t.price, C::round_config(t.tick));
        Eq(a.maker_amount, t.mk, std::string(t.name) + " maker");
        Eq(a.taker_amount, t.tk, std::string(t.name) + " taker");
    }

    std::printf(g_fail ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
