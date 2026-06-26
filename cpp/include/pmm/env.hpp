// pmm/env.hpp — 环境变量读取助手 (port of runner.py 的 _f/_i + os.environ.get 习惯)
//
// 语义与 Python 对齐: 未设置 → default; 设置但解析失败 → default;
//   i() = int(float(x)) (先按浮点解析再截断, 与 _i 一致)。
#pragma once

#include <cstddef>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>

namespace pmm::env {

inline std::string strip(std::string s) {
    const char* ws = " \t\r\n\f\v";  // 与 Python str.strip() 的空白集一致 (含 \f \v)
    const auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

// 原始值 (未 strip)。未设置返回 def。
inline std::string str(const char* name, const std::string& def = "") {
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : def;
}

// 整串解析为 double (允许首尾空白)。有尾部垃圾 → nullopt — 对齐 Python float() 要求整串合法,
// 否则 std::stod 会只取数字前缀 (如 "1000oops"→1000) 而 Python 会抛异常回退默认值。
inline std::optional<double> parse_full(const char* v) {
    const std::string s = strip(std::string(v));
    if (s.empty()) return std::nullopt;
    try {
        std::size_t pos = 0;
        const double d = std::stod(s, &pos);
        if (pos != s.size()) return std::nullopt;  // 尾部还有非数字字符
        return d;
    } catch (...) {
        return std::nullopt;
    }
}

// float(os.environ.get(name, default))，失败/不可解析回退 default。
inline double f(const char* name, double def) {
    const char* v = std::getenv(name);
    if (v == nullptr) return def;
    const auto d = parse_full(v);
    return d ? *d : def;
}

// int(float(os.environ.get(name, default)))，失败回退 default。
inline int i(const char* name, int def) {
    const char* v = std::getenv(name);
    if (v == nullptr) return def;
    const auto d = parse_full(v);
    if (!d) return def;
    // 越界钳位避免 static_cast<int> 的 UB (Python int 任意精度; 超 int 范围按饱和处理)。
    if (*d >= static_cast<double>(std::numeric_limits<int>::max())) return std::numeric_limits<int>::max();
    if (*d <= static_cast<double>(std::numeric_limits<int>::min())) return std::numeric_limits<int>::min();
    return static_cast<int>(*d);
}

// os.environ.get(name, def).strip() == want
inline bool flag_eq(const char* name, const char* want, const char* def) {
    return strip(str(name, def)) == want;
}

// os.environ.get(name, def).strip() != nope
inline bool flag_ne(const char* name, const char* nope, const char* def) {
    return strip(str(name, def)) != nope;
}

}  // namespace pmm::env
