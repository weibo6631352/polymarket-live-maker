// pmm/round.hpp — Python round() 对齐助手 (round-half-to-even, 即 FE_TONEAREST 默认)。
#pragma once

#include <cfenv>
#include <cmath>

namespace pmm {

inline double round_to(double x, int ndigits) {
    double f = 1.0;
    for (int i = 0; i < ndigits; ++i) f *= 10.0;
    return std::nearbyint(x * f) / f;
}

inline double round_int(double x) { return std::nearbyint(x); }

}  // namespace pmm
