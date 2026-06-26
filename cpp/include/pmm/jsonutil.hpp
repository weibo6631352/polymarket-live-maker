// pmm/jsonutil.hpp — nlohmann/json 取值助手 (对齐 Python 的 float()/bool()/str()/.get() 习惯)。
//
// Polymarket 响应字段可能是 number 或 "字符串数字", camelCase 或 snake_case, 数组或 "JSON 字符串"。
#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace pmm::jsonutil {

using nlohmann::json;

// float(v)，失败/不可解析回退 0.0 (Python `float(x or 0)` 习惯)。
inline double to_double(const json& v) {
    if (v.is_number()) return v.get<double>();
    if (v.is_string()) {
        try {
            return std::stod(v.get<std::string>());
        } catch (...) {
            return 0.0;
        }
    }
    return 0.0;
}

// str(v): 字符串原样, null→"", 其余→dump。
inline std::string to_str(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_null()) return "";
    return v.dump();
}

// Python bool(v): 非空字符串=true (注意 "false" 字符串也 true), 数字!=0, 容器非空。
inline bool py_bool(const json& v) {
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_string()) return !v.get<std::string>().empty();
    if (v.is_number()) return v.get<double>() != 0.0;
    if (v.is_array() || v.is_object()) return !v.empty();
    return false;  // null
}

// _to_bool(v): 字符串按 lower=="true" 判定; 其余走 bool(v)。
inline bool str_bool(const json& v) {
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        for (char& c : s) c = static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c);
        return s == "true";
    }
    return py_bool(v);
}

// 真值判断 (Python `x or default` 的 x 部分)。
inline bool truthy(const json& v) { return py_bool(v); }

// 取 key (带可选 fallback key); 不存在或 null 返回 nullptr。
inline const json* find(const json& j, const char* a, const char* b = nullptr) {
    if (!j.is_object()) return nullptr;
    auto it = j.find(a);
    if (it != j.end()) {
        // 主键存在 (即使值为 null): 对齐 Python `data.get(a, data.get(b, default))` —
        // 主键命中就不再看次键 (主键 null → 走默认值, 而不是回退到次键)。
        return it->is_null() ? nullptr : &*it;
    }
    if (b != nullptr) {
        auto it2 = j.find(b);
        if (it2 != j.end() && !it2->is_null()) return &*it2;
    }
    return nullptr;
}

// 字段可能是 "JSON 字符串" 或已是 数组/对象 → 统一成容器 (失败返回空数组)。
inline json as_container(const json& v) {
    if (v.is_string()) {
        try {
            return json::parse(v.get<std::string>());
        } catch (...) {
            return json::array();
        }
    }
    if (v.is_array() || v.is_object()) return v;
    return json::array();
}

}  // namespace pmm::jsonutil
