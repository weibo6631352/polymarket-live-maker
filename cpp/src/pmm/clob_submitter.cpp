// src/pmm/clob_submitter.cpp — 真实 CLOB 下单器实现 (port of pm_trader ClobSubmitter, V2 签名)
#include "pmm/clob_submitter.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include <curl/curl.h>

#include "pmm/crypto/eip712_v2.hpp"
#include "pmm/crypto/secp256k1_signer.hpp"
#include "pmm/env.hpp"
#include "pmm/jsonutil.hpp"
#include "pmm/wire/clob_wire.hpp"

namespace pmm::clob {

namespace ju = pmm::jsonutil;
using nlohmann::json;

namespace {

std::string to_hex(const std::uint8_t* b, std::size_t n, bool prefix = true) {
    static const char* h = "0123456789abcdef";
    std::string s = prefix ? "0x" : "";
    for (std::size_t i = 0; i < n; ++i) {
        s += h[b[i] >> 4];
        s += h[b[i] & 0x0f];
    }
    return s;
}

int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hex_to_bytes(const std::string& hex, std::uint8_t* out, std::size_t n) {
    const char* p = hex.c_str();
    if (hex.size() >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    if (std::strlen(p) != n * 2) return false;
    for (std::size_t i = 0; i < n; ++i) {
        const int hi = hex_val(p[i * 2]);
        const int lo = hex_val(p[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>((c >= 'a' && c <= 'z') ? c - 32 : c);
    return s;
}

std::uint64_t random_salt() {
    std::uint64_t salt = 0;
    const int fd = ::open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        if (::read(fd, &salt, sizeof(salt)) != static_cast<ssize_t>(sizeof(salt))) salt = 0;
        ::close(fd);
    }
    return salt >> 1;  // < 2^63, 正十进制
}

std::size_t write_cb(char* p, std::size_t sz, std::size_t nm, void* ud) {
    static_cast<std::string*>(ud)->append(p, sz * nm);
    return sz * nm;
}

std::int64_t now_unix() { return static_cast<std::int64_t>(::time(nullptr)); }

double mono_now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

double pow10i(int n) {
    double f = 1.0;
    for (int i = 0; i < n; ++i) f *= 10.0;
    return f;
}

}  // namespace

// ---------------------------------------------------------------------------
// 金额计算 (纯函数)
// ---------------------------------------------------------------------------

RoundConfig round_config(const std::string& tick_size) {
    if (tick_size == "0.1") return {1, 2, 3};
    if (tick_size == "0.01") return {2, 2, 4};
    if (tick_size == "0.001") return {3, 2, 5};
    if (tick_size == "0.0001") return {4, 2, 6};
    return {2, 2, 4};
}

double round_normal(double x, int n) { return std::nearbyint(x * pow10i(n)) / pow10i(n); }
double round_down(double x, int n) { return std::floor(x * pow10i(n)) / pow10i(n); }
double round_up(double x, int n) { return std::ceil(x * pow10i(n)) / pow10i(n); }

std::int64_t to_token_decimals(double x) {
    return static_cast<std::int64_t>(std::nearbyint(1'000'000.0 * x));
}

OrderAmounts get_order_amounts(bool is_buy, double size, double price, const RoundConfig& rc) {
    const double raw_price = round_normal(price, rc.price);
    OrderAmounts out;
    if (is_buy) {
        const double raw_taker = round_down(size, rc.size);
        double raw_maker = raw_taker * raw_price;
        raw_maker = round_down(round_up(raw_maker, rc.amount + 4), rc.amount);
        out.maker_amount = static_cast<std::uint64_t>(to_token_decimals(raw_maker));
        out.taker_amount = static_cast<std::uint64_t>(to_token_decimals(raw_taker));
        out.side = 0;
    } else {
        const double raw_maker = round_down(size, rc.size);
        double raw_taker = raw_maker * raw_price;
        raw_taker = round_down(round_up(raw_taker, rc.amount + 4), rc.amount);
        out.maker_amount = static_cast<std::uint64_t>(to_token_decimals(raw_maker));
        out.taker_amount = static_cast<std::uint64_t>(to_token_decimals(raw_taker));
        out.side = 1;
    }
    return out;
}

OrderAmounts get_market_order_amounts(bool is_buy, double amount, double price, const RoundConfig& rc) {
    const double raw_price = round_normal(price, rc.price);
    OrderAmounts out;
    if (is_buy) {  // amount = USDC; shares = USDC / price
        const double raw_maker = round_down(amount, rc.size);
        double raw_taker = raw_price > 0.0 ? raw_maker / raw_price : 0.0;
        raw_taker = round_down(round_up(raw_taker, rc.amount + 4), rc.amount);
        out.maker_amount = static_cast<std::uint64_t>(to_token_decimals(raw_maker));
        out.taker_amount = static_cast<std::uint64_t>(to_token_decimals(raw_taker));
        out.side = 0;
    } else {  // amount = shares; USDC = shares * price
        const double raw_maker = round_down(amount, rc.size);
        double raw_taker = raw_maker * raw_price;
        raw_taker = round_down(round_up(raw_taker, rc.amount + 4), rc.amount);
        out.maker_amount = static_cast<std::uint64_t>(to_token_decimals(raw_maker));
        out.taker_amount = static_cast<std::uint64_t>(to_token_decimals(raw_taker));
        out.side = 1;
    }
    return out;
}

// ---------------------------------------------------------------------------
// ClobSubmitter
// ---------------------------------------------------------------------------

ClobSubmitter::ClobSubmitter(TokenBucket* rate_limiter, std::string endpoint)
    : rate_limiter_(rate_limiter), endpoint_(std::move(endpoint)) {
    // 末尾斜杠去掉。
    while (!endpoint_.empty() && endpoint_.back() == '/') endpoint_.pop_back();

    expiry_s_ = std::max(0.0, pmm::env::f("LM_ORDER_EXPIRY_S", 0.0));
    invert_side_ = pmm::env::flag_eq("POLYMARKET_FILL_SIDE_INVERT", "1", "0");

    if (!pmm::env::flag_eq("PM_TRADER_LIVE", "1", "0")) {
        std::fprintf(stderr, "ClobSubmitter: refusing — set PM_TRADER_LIVE=1 to opt in\n");
        return;
    }
    const std::string pk = pmm::env::str("POLYMARKET_PRIVATE_KEY");
    if (pk.empty() || !hex_to_bytes(pk, creds_.private_key.data(), 32)) {
        std::fprintf(stderr, "ClobSubmitter: POLYMARKET_PRIVATE_KEY missing/invalid\n");
        return;
    }
    crypto::Bytes32 key{};
    std::memcpy(key.data(), creds_.private_key.data(), 32);
    crypto::Address eoa{};
    if (!crypto::DeriveAddress(key, eoa)) {
        std::fprintf(stderr, "ClobSubmitter: cannot derive EOA from private key\n");
        return;
    }
    creds_.signer_lc = to_hex(eoa.data(), 20);
    const std::string funder = pmm::env::str("POLYMARKET_FUNDER");
    creds_.maker = funder.empty() ? creds_.signer_lc : funder;
    creds_.signature_type = pmm::env::i("POLYMARKET_SIGNATURE_TYPE", 0);

    if (expiry_s_ > 0.0) {
        std::fprintf(stderr,
                     "ClobSubmitter: WARNING LM_ORDER_EXPIRY_S=%.0f set but V2 signed order has no "
                     "expiration field — placing GTC (dead-man switch NOT active).\n",
                     expiry_s_);
    }

    std::string err;
    if (!derive_api_creds(err)) {
        std::fprintf(stderr, "ClobSubmitter: derive api creds failed: %s\n", err.c_str());
        return;
    }

    // 把 trade cursor 推到当前最新一笔, 首个 poll_fills 只返回启动后的成交。
    try {
        const auto fills = poll_fills();  // 首次: last_trade_id_ 还空 → 会设到最新并返回这些
        (void)fills;                      // 丢弃启动前的历史 (last_trade_id_ 已就位)
    } catch (...) {
    }

    warmer_ = std::make_unique<pmm::maker::ConnectionWarmer>([this] { warm_ping(); }, 3.0);
    warmer_->start();
    ready_ = true;
}

ClobSubmitter::~ClobSubmitter() {
    close();
    if (curl_ != nullptr) curl_easy_cleanup(static_cast<CURL*>(curl_));
}

void ClobSubmitter::close() {
    if (warmer_) warmer_->stop();
}

void ClobSubmitter::throttle(bool low_priority) {
    if (rate_limiter_ != nullptr) rate_limiter_->acquire(1.0, std::nullopt, low_priority);
}

ClobSubmitter::Resp ClobSubmitter::http(const char* method, const std::string& path,
                                        const std::vector<std::string>& headers,
                                        const std::string& body) {
    std::lock_guard<std::mutex> lk(curl_mu_);  // 持久 handle 非线程安全
    Resp r;
    CURL* c = static_cast<CURL*>(curl_);
    if (c == nullptr) {
        c = curl_easy_init();
        curl_ = c;
    }
    if (c == nullptr) {
        r.body = "curl init failed";
        return r;
    }
    curl_easy_reset(c);  // 重置选项, 保留连接池 → keep-alive 暖复用
    curl_slist* hdr = nullptr;
    for (const auto& h : headers) hdr = curl_slist_append(hdr, h.c_str());
    const std::string url = endpoint_ + path;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    if (std::strcmp(method, "POST") == 0) {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    } else if (std::strcmp(method, "GET") != 0) {
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);  // DELETE
        if (!body.empty()) {
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        }
    }
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 3L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 2L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    const CURLcode rc = curl_easy_perform(c);
    long http_code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(hdr);
    r.status = static_cast<int>(http_code);
    if (rc != CURLE_OK && r.body.empty()) r.body = curl_easy_strerror(rc);
    return r;
}

std::vector<std::string> ClobSubmitter::l2_headers(const std::string& method, const std::string& path,
                                                   const std::string& body,
                                                   const std::string& ts) const {
    const std::string sig = wire::ComputeL2Signature(creds_.api_secret, ts, method, path, body);
    return {"Content-Type: application/json",
            "POLY_ADDRESS: " + creds_.signer_lc,
            "POLY_SIGNATURE: " + sig,
            "POLY_TIMESTAMP: " + ts,
            "POLY_API_KEY: " + creds_.api_key,
            "POLY_PASSPHRASE: " + creds_.api_passphrase};
}

bool ClobSubmitter::derive_api_creds(std::string& err) {
    const std::string ts = std::to_string(now_unix());
    const crypto::Bytes32 digest = wire::ComputeClobAuthDigest(creds_.signer_lc, ts, 0);
    crypto::Bytes32 key{};
    std::memcpy(key.data(), creds_.private_key.data(), 32);
    crypto::Signature65 sig{};
    if (!crypto::SignDigest(digest, key, sig)) {
        err = "L1 sign failed";
        return false;
    }
    const std::vector<std::string> l1 = {"Content-Type: application/json",
                                         "POLY_ADDRESS: " + creds_.signer_lc,
                                         "POLY_SIGNATURE: " + to_hex(sig.data(), 65),
                                         "POLY_TIMESTAMP: " + ts,
                                         "POLY_NONCE: 0"};
    // 先试 create (POST /auth/api-key), 失败再 derive (GET /auth/derive-api-key)。
    Resp r = http("POST", "/auth/api-key", l1, "");
    if (r.status < 200 || r.status >= 300) r = http("GET", "/auth/derive-api-key", l1, "");
    if (r.status < 200 || r.status >= 300) {
        err = "auth " + std::to_string(r.status) + " " + r.body.substr(0, 160);
        return false;
    }
    try {
        const json j = json::parse(r.body);
        if (const json* v = ju::find(j, "apiKey")) creds_.api_key = ju::to_str(*v);
        if (const json* v = ju::find(j, "secret")) creds_.api_secret = ju::to_str(*v);
        if (const json* v = ju::find(j, "passphrase")) creds_.api_passphrase = ju::to_str(*v);
    } catch (...) {
        err = "auth response parse failed";
        return false;
    }
    if (creds_.api_key.empty() || creds_.api_secret.empty()) {
        err = "auth response missing apiKey/secret";
        return false;
    }
    return true;
}

void ClobSubmitter::warm_ping() {
    throttle(/*low_priority=*/true);
    (void)http("GET", "/time", {"Content-Type: application/json"}, "");
}

std::string ClobSubmitter::fetch_tick_size(const std::string& token_id) {
    const auto it = tick_cache_.find(token_id);
    if (it != tick_cache_.end() && it->second.second > mono_now()) return it->second.first;
    throttle(/*low_priority=*/true);
    const Resp r = http("GET", "/tick-size?token_id=" + token_id, {"Content-Type: application/json"}, "");
    std::string tick = "0.01";
    try {
        const json j = json::parse(r.body);
        if (const json* v = ju::find(j, "minimum_tick_size")) tick = ju::to_str(*v);
    } catch (...) {
    }
    tick_cache_[token_id] = {tick, mono_now() + 300.0};
    return tick;
}

bool ClobSubmitter::fetch_neg_risk(const std::string& token_id) {
    const auto it = neg_risk_cache_.find(token_id);
    if (it != neg_risk_cache_.end()) return it->second;
    throttle(/*low_priority=*/true);
    const Resp r = http("GET", "/neg-risk?token_id=" + token_id, {"Content-Type: application/json"}, "");
    bool neg = false;
    try {
        const json j = json::parse(r.body);
        if (const json* v = ju::find(j, "neg_risk")) neg = ju::py_bool(*v);
    } catch (...) {
    }
    neg_risk_cache_[token_id] = neg;
    return neg;
}

std::optional<double> ClobSubmitter::fetch_marketable_price(const std::string& token_id,
                                                            const std::string& side) {
    throttle(/*low_priority=*/true);
    const Resp r =
        http("GET", "/price?token_id=" + token_id + "&side=" + side, {"Content-Type: application/json"}, "");
    try {
        const json j = json::parse(r.body);
        if (const json* v = ju::find(j, "price")) {
            const double p = ju::to_double(*v);
            if (p > 0.0) return p;
        }
    } catch (...) {
    }
    return std::nullopt;
}

json ClobSubmitter::place(const std::string& token_id, const std::string& side, double price,
                          double size) {
    const std::string tick = fetch_tick_size(token_id);
    const bool neg_risk = fetch_neg_risk(token_id);
    const bool is_buy = upper(side) == "BUY";
    const OrderAmounts amt = get_order_amounts(is_buy, size, price, round_config(tick));

    crypto::OrderV2 ord;
    ord.salt = crypto::U256FromU64(random_salt());
    if (!crypto::AddressFromHex(creds_.maker, ord.maker)) return {{"status", "ERROR"}, {"error", "maker addr"}};
    if (!crypto::AddressFromHex(creds_.signer_lc, ord.signer))
        return {{"status", "ERROR"}, {"error", "signer addr"}};
    if (!crypto::U256FromDecimal(token_id, ord.token_id)) return {{"status", "ERROR"}, {"error", "token_id"}};
    ord.maker_amount = amt.maker_amount;
    ord.taker_amount = amt.taker_amount;
    ord.side = static_cast<std::uint8_t>(amt.side);
    ord.signature_type = static_cast<std::uint8_t>(creds_.signature_type);
    const auto now_s = static_cast<std::uint64_t>(now_unix());
    ord.timestamp_ms = now_s * 1000ULL;

    const crypto::Eip712Domain domain = crypto::CtfExchangeV2Domain(neg_risk);
    const crypto::Bytes32 digest = crypto::ComputeOrderV2Digest(ord, domain);
    crypto::Bytes32 key{};
    std::memcpy(key.data(), creds_.private_key.data(), 32);
    crypto::Signature65 sig{};
    if (!crypto::SignDigest(digest, key, sig)) return {{"status", "ERROR"}, {"error", "sign failed"}};
    crypto::Address recovered{};
    if (!crypto::RecoverAddress(digest, sig, recovered) ||
        std::memcmp(recovered.data(), ord.signer.data(), 20) != 0) {
        return {{"status", "ERROR"}, {"error", "recover self-check failed"}};
    }

    wire::OrderV2Wire w;
    {  // salt 低 64 位 (RandomSalt < 2^63)
        std::uint64_t s = 0;
        for (int i = 0; i < 8; ++i) s = (s << 8) | ord.salt[24 + static_cast<std::size_t>(i)];
        w.salt = s;
    }
    w.maker = creds_.maker;
    w.signer = creds_.signer_lc;
    w.token_id = token_id;
    w.maker_amount = amt.maker_amount;
    w.taker_amount = amt.taker_amount;
    w.is_buy = is_buy;
    w.signature_type = static_cast<std::uint32_t>(creds_.signature_type);
    w.timestamp_ms = ord.timestamp_ms;
    w.signature = to_hex(sig.data(), 65);
    w.owner = creds_.api_key;
    w.order_type = "GTC";  // 静息报价 (GTD 在 V2 wire 不可表达, 见构造告警)
    const std::string body = wire::BuildOrderV2Body(w);

    const std::string ts = std::to_string(now_s);
    throttle();
    const Resp r = http("POST", "/order", l2_headers("POST", "/order", body, ts), body);

    json out;
    std::string order_id, status;
    bool success = false, ok_resp = false;
    try {
        const json j = json::parse(r.body);
        if (const json* v = ju::find(j, "orderID")) order_id = ju::to_str(*v);
        else if (const json* v2 = ju::find(j, "order_id")) order_id = ju::to_str(*v2);
        if (const json* v = ju::find(j, "status")) status = ju::to_str(*v);
        if (const json* v = ju::find(j, "success")) success = ju::py_bool(*v);
        ok_resp = true;
    } catch (...) {
    }
    std::string st_lc = status;
    for (char& ch : st_lc) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const bool ok = ok_resp && (r.status >= 200 && r.status < 300) &&
                    (success || !order_id.empty()) && st_lc != "unmatched";
    out["status"] = ok ? "PLACED" : "REJECTED";
    out["order_id"] = order_id;
    out["token_id"] = token_id;
    out["side"] = side;
    out["price"] = price;
    out["size"] = size;
    out["http"] = r.status;
    out["resp"] = r.body;
    return out;
}

json ClobSubmitter::cancel_all(const std::string& token_id) {
    const std::string body = json{{"market", ""}, {"asset_id", token_id}}.dump();
    const std::string ts = std::to_string(now_unix());
    throttle();
    const Resp r =
        http("DELETE", "/cancel-market-orders", l2_headers("DELETE", "/cancel-market-orders", body, ts), body);
    // 网络/超时 (status 0) 或非 2xx → ERROR: 绝不谎报撤单成功 (否则陈旧单留着被挑选, 正是快撤要防的)。
    if (r.status < 200 || r.status >= 300) {
        return {{"status", "ERROR"}, {"error", "cancel_failed"}, {"token_id", token_id},
                {"http", r.status}, {"resp", r.body}};
    }
    return {{"status", "CANCELLED"}, {"token_id", token_id}, {"http", r.status}, {"resp", r.body}};
}

json ClobSubmitter::flatten(const std::string& token_id, const std::string& side, double size) {
    const std::string tick = fetch_tick_size(token_id);
    const RoundConfig rc = round_config(tick);
    const bool is_buy = upper(side) == "BUY";
    // 拿不到可成交价 → 不发错价单 (fail closed: 仓位存活, 下轮重试)。原来 SELL 回退 0.01 会愿以 ~1c 抛货。
    const auto mkt_px = fetch_marketable_price(token_id, is_buy ? "BUY" : "SELL");
    if (!mkt_px) {
        return {{"status", "ERROR"}, {"error", "flatten_no_price"}, {"token_id", token_id},
                {"side", side}, {"size", size}};
    }
    const double px = *mkt_px;
    const OrderAmounts amt = is_buy ? get_market_order_amounts(true, size * px, px, rc)  // 空头回补: amount=USDC
                                    : get_market_order_amounts(false, size, px, rc);     // 多头平仓: amount=shares
    const bool neg_risk = fetch_neg_risk(token_id);

    crypto::OrderV2 ord;
    ord.salt = crypto::U256FromU64(random_salt());
    if (!crypto::AddressFromHex(creds_.maker, ord.maker)) return {{"status", "ERROR"}, {"error", "maker addr"}};
    if (!crypto::AddressFromHex(creds_.signer_lc, ord.signer))
        return {{"status", "ERROR"}, {"error", "signer addr"}};
    if (!crypto::U256FromDecimal(token_id, ord.token_id)) return {{"status", "ERROR"}, {"error", "token_id"}};
    ord.maker_amount = amt.maker_amount;
    ord.taker_amount = amt.taker_amount;
    ord.side = static_cast<std::uint8_t>(amt.side);
    ord.signature_type = static_cast<std::uint8_t>(creds_.signature_type);
    const auto now_s = static_cast<std::uint64_t>(now_unix());
    ord.timestamp_ms = now_s * 1000ULL;

    const crypto::Eip712Domain domain = crypto::CtfExchangeV2Domain(neg_risk);
    const crypto::Bytes32 digest = crypto::ComputeOrderV2Digest(ord, domain);
    crypto::Bytes32 key{};
    std::memcpy(key.data(), creds_.private_key.data(), 32);
    crypto::Signature65 sig{};
    if (!crypto::SignDigest(digest, key, sig)) return {{"status", "ERROR"}, {"error", "sign failed"}};

    wire::OrderV2Wire w;
    {
        std::uint64_t s = 0;
        for (int i = 0; i < 8; ++i) s = (s << 8) | ord.salt[24 + static_cast<std::size_t>(i)];
        w.salt = s;
    }
    w.maker = creds_.maker;
    w.signer = creds_.signer_lc;
    w.token_id = token_id;
    w.maker_amount = amt.maker_amount;
    w.taker_amount = amt.taker_amount;
    w.is_buy = is_buy;
    w.signature_type = static_cast<std::uint32_t>(creds_.signature_type);
    w.timestamp_ms = ord.timestamp_ms;
    w.signature = to_hex(sig.data(), 65);
    w.owner = creds_.api_key;
    w.order_type = "FOK";  // 平仓全成或全不成
    const std::string body = wire::BuildOrderV2Body(w);

    const std::string ts = std::to_string(now_s);
    throttle();
    const Resp r = http("POST", "/order", l2_headers("POST", "/order", body, ts), body);

    std::string status, order_id;
    bool ok_resp = false;
    try {
        const json j = json::parse(r.body);
        if (const json* v = ju::find(j, "status")) status = ju::to_str(*v);
        if (const json* v = ju::find(j, "orderID")) order_id = ju::to_str(*v);
        ok_resp = true;
    } catch (...) {
    }
    std::string st_lc = status;
    for (char& ch : st_lc) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const bool filled = ok_resp && (st_lc == "matched" || st_lc == "delayed");
    if (!filled) {  // FOK 未成 → 仓位存活, fail closed
        return {{"status", "ERROR"}, {"error", "flatten_unfilled"}, {"token_id", token_id},
                {"side", side},      {"size", size},                {"http", r.status},
                {"resp", r.body}};
    }
    if (!order_id.empty()) {
        own_taker_ids_.insert(order_id);
        own_taker_fifo_.push_back(order_id);
        if (own_taker_fifo_.size() > 500) {  // 真 FIFO: 淘汰最旧, 保留刚加入的 (防自成交腿被误算为 maker fill)
            own_taker_ids_.erase(own_taker_fifo_.front());
            own_taker_fifo_.pop_front();
        }
    }
    return {{"status", "FLATTENED"}, {"token_id", token_id}, {"side", side},
            {"size", size},          {"order_id", order_id}, {"http", r.status}};
}

nlohmann::json ClobSubmitter::operator()(const nlohmann::json& action) noexcept {
    try {
        const std::string kind = ju::find(action, "action") ? ju::to_str(*ju::find(action, "action")) : "";
        const std::string token = ju::find(action, "token_id") ? ju::to_str(*ju::find(action, "token_id")) : "";
        if (kind == "PLACE") {
            return place(token, ju::to_str(*ju::find(action, "side")), ju::to_double(*ju::find(action, "price")),
                         ju::to_double(*ju::find(action, "size")));
        }
        if (kind == "CANCEL_ALL") return cancel_all(token);
        if (kind == "FLATTEN") {
            return flatten(token, ju::to_str(*ju::find(action, "side")), ju::to_double(*ju::find(action, "size")));
        }
    } catch (const std::exception& e) {
        json r = action;
        r["status"] = "ERROR";
        r["error"] = e.what();
        return r;
    } catch (...) {
        json r = action;
        r["status"] = "ERROR";
        r["error"] = "unknown";
        return r;
    }
    json r = action;
    r["status"] = "IGNORED";
    return r;
}

std::vector<json> ClobSubmitter::poll_fills() {
    const std::string path = "/data/trades";
    // 两个叠加的 bug 都修: (1) 必须按 ORDER MAKER 查 (sig_type=1 = funder/proxy), 不是 signer;
    // (2) 去掉 &next_cursor=MA== —— 它让查询返回空 (诊断: 带它 0 条, 不带它 300 条)。增量靠 last_trade_id_ 截断。
    std::string maker_lc = creds_.maker;
    for (char& c : maker_lc) c = static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c);
    const std::string query = path + "?maker_address=" + maker_lc;
    const std::string ts = std::to_string(now_unix());
    throttle(/*low_priority=*/true);
    const Resp r = http("GET", query, l2_headers("GET", path, "", ts), "");

    std::vector<json> out;
    json data;
    try {
        const json j = json::parse(r.body);
        if (j.is_array()) {
            data = j;
        } else if (const json* d = ju::find(j, "data")) {
            data = *d;
        }
    } catch (...) {
        return out;
    }
    if (!data.is_array()) return out;

    bool seen_new = false;
    for (const auto& t : data) {  // newest-first
        std::string tid;
        if (const json* v = ju::find(t, "id")) tid = ju::to_str(*v);
        else if (const json* v2 = ju::find(t, "trade_id")) tid = ju::to_str(*v2);
        if (!tid.empty() && last_trade_id_.has_value() && tid == *last_trade_id_) break;
        if (!seen_new && !tid.empty()) {
            last_trade_id_ = tid;
            seen_new = true;
        }
        // 排除自己的 flatten(taker) 腿
        bool own = false;
        for (const char* k : {"taker_order_id", "takerOrderId", "order_id", "orderID"}) {
            if (const json* v = ju::find(t, k)) {
                if (own_taker_ids_.count(ju::to_str(*v)) != 0) {
                    own = true;
                    break;
                }
            }
        }
        if (own) continue;
        std::string side;
        if (const json* v = ju::find(t, "side")) side = upper(ju::to_str(*v));
        if (invert_side_) side = (side == "BUY") ? "SELL" : "BUY";
        std::string token;
        if (const json* v = ju::find(t, "asset_id")) token = ju::to_str(*v);
        else if (const json* v2 = ju::find(t, "token_id")) token = ju::to_str(*v2);
        json f;
        f["id"] = tid;
        f["token_id"] = token;
        f["side"] = side;
        f["size"] = (ju::find(t, "size") != nullptr) ? ju::to_double(*ju::find(t, "size")) : 0.0;
        f["price"] = (ju::find(t, "price") != nullptr) ? ju::to_double(*ju::find(t, "price")) : 0.0;
        out.push_back(std::move(f));
    }
    return out;
}

json ClobSubmitter::debug_trades() {
    const std::string path = "/data/trades";
    std::string maker_lc = creds_.maker;
    for (char& c : maker_lc) c = static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c);
    const std::string ts = std::to_string(now_unix());
    const std::vector<std::pair<std::string, std::string>> variants = {
        {"maker=funder_asis", path + "?maker_address=" + creds_.maker},
        {"maker=funder_lc", path + "?maker_address=" + maker_lc},
        {"maker=signer", path + "?maker_address=" + creds_.signer_lc},
        {"taker=funder_lc", path + "?taker_address=" + maker_lc},
        {"nofilter", path},
    };
    json out = json::array();
    for (const auto& [label, query] : variants) {
        throttle(/*low_priority=*/true);
        const Resp r = http("GET", query, l2_headers("GET", path, "", ts), "");
        std::size_t n = 0;
        try {
            const json j = json::parse(r.body);
            if (j.is_array())
                n = j.size();
            else if (const json* d = ju::find(j, "data"))
                n = d->is_array() ? d->size() : 0;
        } catch (...) {
        }
        out.push_back({{"variant", label}, {"http", r.status}, {"count", n}, {"body", r.body.substr(0, 200)}});
    }
    return out;
}

std::optional<double> ClobSubmitter::usdc_balance() {
    const std::string path = "/balance-allowance";
    const std::string query = path + "?asset_type=COLLATERAL&signature_type=" +
                              std::to_string(creds_.signature_type);
    const std::string ts = std::to_string(now_unix());
    throttle(/*low_priority=*/true);
    const Resp r = http("GET", query, l2_headers("GET", path, "", ts), "");
    try {
        const json j = json::parse(r.body);
        if (const json* v = ju::find(j, "balance")) {
            const double bal = ju::to_double(*v);
            return bal / 1'000'000.0;  // USDC 6 decimals
        }
    } catch (...) {
    }
    return std::nullopt;
}

json ClobSubmitter::api_creds() {
    return {{"apiKey", creds_.api_key}, {"secret", creds_.api_secret}, {"passphrase", creds_.api_passphrase}};
}

std::vector<json> ClobSubmitter::list_open_orders() {
    const std::string path = "/data/orders";
    const std::string ts = std::to_string(now_unix());
    throttle(/*low_priority=*/true);
    const Resp r = http("GET", path, l2_headers("GET", path, "", ts), "");
    std::vector<json> out;
    try {
        const json j = json::parse(r.body);
        const json* d = ju::find(j, "data");
        const json arr = j.is_array() ? j : (d != nullptr ? *d : json::array());
        if (arr.is_array()) {
            for (const auto& o : arr) out.push_back(o);
        }
    } catch (...) {
    }
    return out;
}

void ClobSubmitter::cancel_order(const std::string& order_id) {
    const std::string body = json{{"orderID", order_id}}.dump();
    const std::string ts = std::to_string(now_unix());
    throttle();
    (void)http("DELETE", "/order", l2_headers("DELETE", "/order", body, ts), body);
}

}  // namespace pmm::clob
