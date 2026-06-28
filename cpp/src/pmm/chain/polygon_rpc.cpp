// pmm/chain/polygon_rpc.cpp — 极简 Polygon JSON-RPC 客户端实现 (libcurl)。
#include "pmm/chain/polygon_rpc.hpp"

#include <cstdlib>
#include <cstring>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "pmm/jsonutil.hpp"

namespace pmm::chain {

namespace ju = pmm::jsonutil;
using nlohmann::json;

namespace {

std::size_t write_cb(char* p, std::size_t sz, std::size_t nm, void* ud) {
    static_cast<std::string*>(ud)->append(p, sz * nm);
    return sz * nm;
}

std::string to_hex(const std::vector<std::uint8_t>& b) {
    static const char* h = "0123456789abcdef";
    std::string s = "0x";
    s.reserve(2 + b.size() * 2);
    for (std::uint8_t x : b) {
        s += h[x >> 4];
        s += h[x & 0x0f];
    }
    return s;
}

// 解析 "0x.." 数量 (hex) → uint64。空/非法 → nullopt。
std::optional<std::uint64_t> parse_hex_qty(const std::string& s) {
    const char* p = s.c_str();
    if (s.size() >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    if (*p == '\0') return std::nullopt;
    std::uint64_t v = 0;
    for (; *p != '\0'; ++p) {
        v <<= 4;
        if (*p >= '0' && *p <= '9') v |= static_cast<std::uint64_t>(*p - '0');
        else if (*p >= 'a' && *p <= 'f') v |= static_cast<std::uint64_t>(*p - 'a' + 10);
        else if (*p >= 'A' && *p <= 'F') v |= static_cast<std::uint64_t>(*p - 'A' + 10);
        else return std::nullopt;
    }
    return v;
}

}  // namespace

PolygonRpc::PolygonRpc(std::string url, int timeout_ms) : url_(std::move(url)), timeout_ms_(timeout_ms) {
    while (!url_.empty() && url_.back() == '/') url_.pop_back();
}

PolygonRpc::~PolygonRpc() {
    if (curl_ != nullptr) curl_easy_cleanup(static_cast<CURL*>(curl_));
}

RpcResult PolygonRpc::Call(const std::string& method, const std::string& params_json) {
    RpcResult r;
    if (url_.empty()) {
        r.value = "no rpc url";
        return r;
    }
    CURL* c = static_cast<CURL*>(curl_);
    if (c == nullptr) {
        c = curl_easy_init();
        curl_ = c;
    }
    if (c == nullptr) {
        r.value = "curl init failed";
        return r;
    }
    curl_easy_reset(c);
    const std::string body =
        R"({"jsonrpc":"2.0","id":1,"method":")" + method + R"(","params":)" + params_json + "}";
    std::string resp;
    curl_slist* hdr = curl_slist_append(nullptr, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_URL, url_.c_str());
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms_));
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    const CURLcode rc = curl_easy_perform(c);
    long http_code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(hdr);
    r.http = static_cast<int>(http_code);
    if (rc != CURLE_OK) {
        r.value = curl_easy_strerror(rc);
        return r;
    }
    try {
        const json j = json::parse(resp);
        if (const json* e = ju::find(j, "error")) {
            r.value = e->dump();
            return r;
        }
        if (const json* v = ju::find(j, "result")) {
            r.ok = true;
            r.value = v->is_string() ? v->get<std::string>() : v->dump();
            return r;
        }
        r.value = "no result/error in response";
    } catch (...) {
        r.value = "json parse failed";
    }
    return r;
}

std::optional<std::uint64_t> PolygonRpc::ChainId() {
    const RpcResult r = Call("eth_chainId", "[]");
    if (!r.ok) return std::nullopt;
    return parse_hex_qty(r.value);
}

std::optional<std::uint64_t> PolygonRpc::TransactionCount(const std::string& address_hex) {
    const RpcResult r = Call("eth_getTransactionCount", R"([")" + address_hex + R"(","pending"])");
    if (!r.ok) return std::nullopt;
    return parse_hex_qty(r.value);
}

std::optional<std::uint64_t> PolygonRpc::MaxPriorityFeePerGas() {
    const RpcResult r = Call("eth_maxPriorityFeePerGas", "[]");
    if (!r.ok) return std::nullopt;
    return parse_hex_qty(r.value);
}

std::optional<std::uint64_t> PolygonRpc::GasPrice() {
    const RpcResult r = Call("eth_gasPrice", "[]");
    if (!r.ok) return std::nullopt;
    return parse_hex_qty(r.value);
}

std::optional<std::uint64_t> PolygonRpc::EstimateGas(const std::string& from_hex,
                                                     const std::string& to_addr,
                                                     const std::vector<std::uint8_t>& data,
                                                     std::uint64_t value) {
    json call = {{"from", from_hex}, {"to", to_addr}, {"data", to_hex(data)}};
    (void)value;  // value=0 for merge; omit to keep payload minimal
    const std::string params = "[" + call.dump() + R"(,"pending"])";
    const RpcResult r = Call("eth_estimateGas", params);
    if (!r.ok) return std::nullopt;
    return parse_hex_qty(r.value);
}

RpcResult PolygonRpc::SendRawTransaction(const std::vector<std::uint8_t>& raw) {
    // !! 真实上链 !! 仅由 MergeExecutor 双闸全开 + 用户监督下调用。
    return Call("eth_sendRawTransaction", R"([")" + to_hex(raw) + R"("])");
}

}  // namespace pmm::chain
