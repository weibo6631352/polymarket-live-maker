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
#include <thread>

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

// 从下单 response 解析"实际成交股数": 仅当 status=matched/delayed 才计。REJECTED/其它的 making/takingAmount
// 是订单"意向量"非"成交量" — 误读会把"被拒的平仓"当成成功 → 置 inv=0 退场 → 真仓孤立 (实测孤立根因)。
// BUY(回补)看 takingAmount(买入股数), SELL(平多)看 makingAmount(卖出股数)。
double order_filled_shares(const json& j, bool is_buy) {
    std::string st;
    if (const json* v = ju::find(j, "status")) st = ju::to_str(*v);
    for (char& ch : st) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (st != "matched" && st != "delayed") return 0.0;
    const json* v = ju::find(j, is_buy ? "takingAmount" : "makingAmount");
    if (v == nullptr) return 0.0;
    if (v->is_number()) return v->get<double>();
    if (v->is_string()) {
        try {
            return std::stod(v->get<std::string>());
        } catch (...) {
        }
    }
    return 0.0;
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
// +eps/-eps 吸收浮点残差: 否则 10.2 的 double 表示 (10.19999...) 被 floor 成 10.19999 → 隐含价 0.5099995
// 破坏 0.001 tick → 下单被拒 (实测 NO 腿反复被拒)。eps 远大于 FP 误差、远小于 1 个最小计价单位, 不动真值。
double round_down(double x, int n) { return std::floor(x * pow10i(n) + 1e-6) / pow10i(n); }
double round_up(double x, int n) { return std::ceil(x * pow10i(n) - 1e-6) / pow10i(n); }

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

ClobSubmitter::ClobSubmitter(RateLimiter* rate_limiter, std::string endpoint)
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

ClobSubmitter::Resp ClobSubmitter::http(const char* method, const std::string& path,
                                        const std::vector<std::string>& headers,
                                        const std::string& body) {
    // per-endpoint 限速 (取 token 阻塞) 必须在拿 curl 锁之前, 否则等额度时会卡住别的请求。
    if (rate_limiter_ != nullptr) rate_limiter_->acquire(path, method);
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
    (void)http("GET", "/time", {"Content-Type: application/json"}, "");
}

std::string ClobSubmitter::fetch_tick_size(const std::string& token_id) {
    const auto it = tick_cache_.find(token_id);
    if (it != tick_cache_.end() && it->second.second > mono_now()) return it->second.first;
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
                          double size, const std::string& order_type) {
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
    // GTC = resting quote (default); FAK = fill-and-kill (marketable entry that can't rest -> no async orphan).
    w.order_type = (order_type == "FAK" || order_type == "FOK") ? order_type : "GTC";  // GTD 在 V2 wire 不可表达
    const std::string body = wire::BuildOrderV2Body(w);

    const std::string ts = std::to_string(now_s);
    const Resp r = http("POST", "/order", l2_headers("POST", "/order", body, ts), body);

    json out;
    std::string order_id, status;
    double filled = 0.0;
    bool success = false, ok_resp = false;
    try {
        const json j = json::parse(r.body);
        if (const json* v = ju::find(j, "orderID")) order_id = ju::to_str(*v);
        else if (const json* v2 = ju::find(j, "order_id")) order_id = ju::to_str(*v2);
        if (const json* v = ju::find(j, "status")) status = ju::to_str(*v);
        if (const json* v = ju::find(j, "success")) success = ju::py_bool(*v);
        filled = order_filled_shares(j, is_buy);  // ACTUAL filled shares (a marketable buy may partial-fill < size)
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
    out["filled"] = filled;  // actual matched shares — sell THIS, not the intended size (partial-fill bug)
    out["http"] = r.status;
    out["resp"] = r.body;
    return out;
}

json ClobSubmitter::cancel_all(const std::string& token_id) {
    const std::string body = json{{"market", ""}, {"asset_id", token_id}}.dump();
    const std::string ts = std::to_string(now_unix());
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
    const bool neg_risk = fetch_neg_risk(token_id);
    double tick_d = 0.01;
    try {
        tick_d = std::stod(tick);
    } catch (...) {
    }
    // 重试到清零: FAK 在薄盘口只吃顶档 → 部分成交 → 残量孤立 (实测: 247 股大单被扫后只平掉一点,
    // 剩 247 股孤立, 手动平时价格已跌→亏)。循环最多 N 次, 每次重新取价+扫剩余量, 直到仓位清掉。
    // 成交量由 order_filled_shares 解析 (仅 matched 才计 — REJECTED 不算, 修"被拒当成功"的孤立根因)。
    constexpr int kMaxAttempts = 6;
    double remaining = size;
    double total_filled = 0.0;
    std::string last_status, last_order_id, last_resp;
    int last_http = 0;
    for (int attempt = 1; attempt <= kMaxAttempts && remaining >= 1.0; ++attempt) {
        // 拿不到可成交价 → 不发错价单 (fail closed); 等盘口后重试。
        const auto mkt_px = fetch_marketable_price(token_id, is_buy ? "BUY" : "SELL");
        if (!mkt_px) {
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            continue;
        }
        // 扫单价: 阶梯式激进 (第 attempt 次扫 2/4/6/8… 档, 封顶 8)。流动盘口第一次 2 档就吃满 → 省穿价
        // 滑点; 薄盘口/跳变下随重试逐步加深保证清零。比固定 8 档每次都付满滑点省钱 (mid 不可预测, 不能持有等)。
        const double sweep = std::min(8.0, 2.0 * static_cast<double>(attempt)) * tick_d;
        double px = *mkt_px;
        if (is_buy)
            px = std::min(1.0 - tick_d, px + sweep);  // 空头回补: 抬价扫 ask
        else
            px = std::max(tick_d, px - sweep);  // 平多头: 压价扫 bid
        const OrderAmounts amt = is_buy ? get_market_order_amounts(true, remaining * px, px, rc)
                                        : get_market_order_amounts(false, remaining, px, rc);

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
        w.order_type = "FAK";  // fill-and-kill: 吃掉可成交的, 余量取消
        const std::string body = wire::BuildOrderV2Body(w);
        const std::string ts = std::to_string(now_s);
        const Resp r = http("POST", "/order", l2_headers("POST", "/order", body, ts), body);
        last_http = r.status;
        last_resp = r.body;

        double made = 0.0;
        try {
            const json j = json::parse(r.body);
            if (const json* v = ju::find(j, "status")) last_status = ju::to_str(*v);
            if (const json* v = ju::find(j, "orderID")) last_order_id = ju::to_str(*v);
            made = order_filled_shares(j, is_buy);  // 仅 matched 才计 (REJECTED 的 making/takingAmount 是意向量)
        } catch (...) {
        }
        if (made > 0.0) {
            total_filled += made;
            remaining -= made;
            if (!last_order_id.empty()) {  // 排除自成交腿被误算为 maker fill
                own_taker_ids_.insert(last_order_id);
                own_taker_fifo_.push_back(last_order_id);
                if (own_taker_fifo_.size() > 500) {
                    own_taker_ids_.erase(own_taker_fifo_.front());
                    own_taker_fifo_.pop_front();
                }
            }
        }
        if (remaining < 1.0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));  // 等盘口回补再扫剩余
    }
    if (remaining < 1.0) {  // 清零 (或剩 <1 股零头)
        return {{"status", "FLATTENED"}, {"token_id", token_id}, {"side", side},   {"size", size},
                {"filled", total_filled}, {"order_id", last_order_id}, {"http", last_http}};
    }
    // 重试 N 次仍有残量 → 上报, caller 大声告警/记孤立 (fail closed)。
    return {{"status", "ERROR"},      {"error", "flatten_partial"}, {"token_id", token_id},
            {"side", side},           {"size", size},               {"filled", total_filled},
            {"remaining", remaining}, {"http", last_http},          {"resp", last_resp}};
}

nlohmann::json ClobSubmitter::operator()(const nlohmann::json& action) noexcept {
    try {
        const std::string kind = ju::find(action, "action") ? ju::to_str(*ju::find(action, "action")) : "";
        const std::string token = ju::find(action, "token_id") ? ju::to_str(*ju::find(action, "token_id")) : "";
        if (kind == "PLACE") {
            const std::string ot = ju::find(action, "order_type") ? ju::to_str(*ju::find(action, "order_type")) : "GTC";
            return place(token, ju::to_str(*ju::find(action, "side")), ju::to_double(*ju::find(action, "price")),
                         ju::to_double(*ju::find(action, "size")), ot);
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

json ClobSubmitter::fetch_trades_raw() {
    const std::string path = "/data/trades";
    // 两个叠加的 bug 都修: (1) 必须按 ORDER MAKER 查 (sig_type=1 = funder/proxy), 不是 signer;
    // (2) 去掉 &next_cursor=MA== —— 它让查询返回空 (诊断: 带它 0 条, 不带它 300 条)。增量靠游标截断。
    std::string maker_lc = creds_.maker;
    for (char& c : maker_lc) c = static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c);
    const std::string query = path + "?maker_address=" + maker_lc;
    const std::string ts = std::to_string(now_unix());
    const Resp r = http("GET", query, l2_headers("GET", path, "", ts), "");
    try {
        const json j = json::parse(r.body);
        if (j.is_array()) return j;
        if (const json* d = ju::find(j, "data")) return *d;
    } catch (...) {
    }
    return json();
}

std::vector<json> ClobSubmitter::poll_fills() {
    const json data = fetch_trades_raw();
    if (!data.is_array()) return {};
    return extract_new_fills(data, last_trade_id_, own_taker_ids_, invert_side_);
}

std::vector<json> ClobSubmitter::fills_since(const std::string& from_id) {
    if (from_id.empty()) return {};
    const json data = fetch_trades_raw();
    if (!data.is_array()) return {};
    std::optional<std::string> cur = from_id;
    return extract_new_fills(data, cur, own_taker_ids_, invert_side_);
}

std::vector<json> ClobSubmitter::extract_new_fills(const json& data, std::optional<std::string>& last_id,
                                                   const std::set<std::string>& own_taker,
                                                   bool invert_side) {
    std::vector<json> out;
    if (!data.is_array()) return out;
    // 进入本轮时的旧游标 (上一轮的最新一笔)。break 必须对它比 —— 不能对循环里刚被推进的 last_id 比,
    // 否则第 1 条就把 last_id 设成本轮最新, 第 2 条起永远不等于它 → 永不 break → 返回全部 trades,
    // 把 runner 去重 (cap) 撑爆 → 旧成交被反复当新 → 库存账本错乱 → 仓位孤立。(根因: 实测两次孤立事故)
    const std::optional<std::string> prev_id = last_id;
    bool seen_new = false;
    for (const auto& t : data) {  // newest-first
        std::string tid;
        if (const json* v = ju::find(t, "id")) tid = ju::to_str(*v);
        else if (const json* v2 = ju::find(t, "trade_id")) tid = ju::to_str(*v2);
        if (!tid.empty() && prev_id.has_value() && tid == *prev_id) break;  // 到上轮最新一笔 → 停
        if (!seen_new && !tid.empty()) {
            last_id = tid;  // 游标推进到本轮最新
            seen_new = true;
        }
        // 排除自己的 flatten(taker) 腿
        bool own = false;
        for (const char* k : {"taker_order_id", "takerOrderId", "order_id", "orderID"}) {
            if (const json* v = ju::find(t, k)) {
                if (own_taker.count(ju::to_str(*v)) != 0) {
                    own = true;
                    break;
                }
            }
        }
        if (own) continue;
        std::string side;
        if (const json* v = ju::find(t, "side")) side = upper(ju::to_str(*v));
        if (invert_side) side = (side == "BUY") ? "SELL" : "BUY";
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

json ClobSubmitter::recent_trades_raw(std::size_t n) {
    const json data = fetch_trades_raw();
    if (!data.is_array()) return json::array();
    json out = json::array();
    for (const auto& t : data) {
        if (out.size() >= n) break;
        out.push_back(t);
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

json ClobSubmitter::query_rewards(const std::string& date) {
    std::string maker_lc = creds_.maker;
    for (char& c : maker_lc) c = static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c);
    const std::string st = std::to_string(creds_.signature_type);
    json out;
    // 1) /rewards/user/markets — 真实已赚 earnings + earning_percentage + rewards_config (按 date)。
    {
        const std::string path = "/rewards/user/markets";
        // order_by=earnings DESC + 大 page_size: 把我们有收益的市场顶到首页 (否则埋在 8000+ 个市场里翻不到)。
        std::string query = path + "?maker_address=" + maker_lc + "&signature_type=" + st +
                            "&order_by=earnings&position=DESC&page_size=500";
        if (!date.empty()) query += "&date=" + date;
        const std::string ts = std::to_string(now_unix());
        const Resp r = http("GET", query, l2_headers("GET", path, "", ts), "");
        out["markets_http"] = r.status;
        // 解析汇总 (响应 8000+ 市场/几百 KB, 不回原始): 真实 accrued 总额 + top earners。
        double accrued = 0.0;
        int n_earning = 0;
        json top = json::array();
        json by_market = json::array();  // 全部 earners {cond, earn, pct} — 供校准 per-pool 联结
        try {
            const json j = json::parse(r.body);
            const json* data = ju::find(j, "data");
            if (data != nullptr && data->is_array()) {
                for (const auto& m : *data) {
                    double e = 0.0;
                    if (const json* earr = ju::find(m, "earnings")) {
                        if (earr->is_array())
                            for (const auto& x : *earr)
                                if (const json* ev = ju::find(x, "earnings")) e += ju::to_double(*ev);
                    }
                    if (e > 1e-9) {
                        ++n_earning;
                        accrued += e;
                        const json* cv = ju::find(m, "condition_id");
                        const json* pct = ju::find(m, "earning_percentage");
                        const double pctv = pct != nullptr ? ju::to_double(*pct) : 0.0;
                        if (by_market.size() < 300)
                            by_market.push_back({{"cond", cv != nullptr ? ju::to_str(*cv) : ""},
                                                 {"earn", std::nearbyint(e * 1e4) / 1e4},
                                                 {"pct", pctv}});
                        if (top.size() < 8) {
                            const json* q = ju::find(m, "question");
                            top.push_back({{"earn", std::nearbyint(e * 1e4) / 1e4},
                                           {"pct", pctv},
                                           {"q", q != nullptr ? ju::to_str(*q).substr(0, 40) : ""}});
                        }
                    }
                }
            }
        } catch (...) {
        }
        out["accrued_total"] = std::nearbyint(accrued * 1e4) / 1e4;
        out["earning_markets"] = n_earning;
        out["top_earners"] = top;
        out["by_market"] = by_market;
    }
    // 2) /rewards/user/percentages — 实时占比 {condition_id: %}; 只回条数 + 非零项。
    {
        const std::string path = "/rewards/user/percentages";
        const std::string query = path + "?maker_address=" + maker_lc + "&signature_type=" + st;
        const std::string ts = std::to_string(now_unix());
        const Resp r = http("GET", query, l2_headers("GET", path, "", ts), "");
        out["percentages_http"] = r.status;
        int live = 0;
        json by_cond = json::object();  // {condition_id: 实时占比%} — 供复评按真实占比踢死池
        try {
            const json j = json::parse(r.body);
            if (j.is_object())
                for (auto it = j.begin(); it != j.end(); ++it)
                    if (it.value().is_number()) {
                        const double pv = ju::to_double(it.value());
                        by_cond[it.key()] = pv;
                        if (pv > 0.0) ++live;
                    }
        } catch (...) {
        }
        out["live_pct_markets"] = live;
        out["pct_by_condition"] = by_cond;
    }
    // 3) /rewards/user/total — 官方汇总总额 (按 asset 分组), 权威 ground truth。比 #1 自己逐行求和可靠:
    //    /markets 的 earnings 是日内估计 (会在结算时修正); /total 是官方结算口径。校准利润模型以此为准。
    {
        const std::string path = "/rewards/user/total";
        std::string query = path + "?maker_address=" + maker_lc + "&signature_type=" + st;
        if (!date.empty()) query += "&date=" + date;
        const std::string ts = std::to_string(now_unix());
        const Resp r = http("GET", query, l2_headers("GET", path, "", ts), "");
        out["total_http"] = r.status;
        double official = 0.0;
        try {
            const json j = json::parse(r.body);  // 数组 [{date, asset_address, earnings, asset_rate}]
            const json* arr = j.is_array() ? &j : ju::find(j, "data");
            if (arr != nullptr && arr->is_array())
                for (const auto& t : *arr) {
                    double e = 0.0, rate = 1.0;
                    if (const json* ev = ju::find(t, "earnings")) e = ju::to_double(*ev);
                    if (const json* rv = ju::find(t, "asset_rate")) rate = ju::to_double(*rv);
                    official += e * (rate > 0.0 ? rate : 1.0);  // 折成 USDC (asset_rate≈0.999)
                }
        } catch (...) {
        }
        out["official_total"] = std::nearbyint(official * 1e4) / 1e4;
    }
    return out;
}

std::optional<double> ClobSubmitter::usdc_balance() {
    const std::string path = "/balance-allowance";
    const std::string query = path + "?asset_type=COLLATERAL&signature_type=" +
                              std::to_string(creds_.signature_type);
    const std::string ts = std::to_string(now_unix());
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
    (void)http("DELETE", "/order", l2_headers("DELETE", "/order", body, ts), body);
}

}  // namespace pmm::clob
