// pmm/strutil.hpp — 字符串助手 (对齐 Python 的 Unicode 语义)。
#pragma once

#include <cstddef>
#include <string>

namespace pmm::strutil {

// 取前 max_cp 个 UTF-8 码点 (对齐 Python s[:n] 的码点切片; 永远切在码点边界, 不产生非法 UTF-8)。
inline std::string utf8_prefix(const std::string& s, std::size_t max_cp) {
    std::size_t i = 0;
    std::size_t cp = 0;
    while (i < s.size() && cp < max_cp) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t len = 1;
        if (c >= 0xF0) {
            len = 4;
        } else if (c >= 0xE0) {
            len = 3;
        } else if (c >= 0xC0) {
            len = 2;
        }
        i += len;
        ++cp;
    }
    if (i > s.size()) i = s.size();  // 末尾不完整序列: 夹到串长
    return s.substr(0, i);
}

}  // namespace pmm::strutil
