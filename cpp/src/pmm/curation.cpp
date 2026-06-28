// src/pmm/curation.cpp — 自主 LLM 选池实现 (Anthropic Claude /v1/messages, 复用 libcurl + nlohmann::json)
#include "pmm/curation.hpp"

#include <cstddef>
#include <cstdio>
#include <string>

#include <curl/curl.h>

#include "pmm/jsonutil.hpp"

namespace pmm::curation {

namespace ju = pmm::jsonutil;
using nlohmann::json;

namespace {

std::size_t write_cb(char* p, std::size_t sz, std::size_t nm, void* ud) {
    static_cast<std::string*>(ud)->append(p, sz * nm);
    return sz * nm;
}

// mid 紧凑成 2 位小数 (prompt 体积小, 不丢做市决策所需精度)。
std::string fmt_mid(double mid) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.2f", mid);
    return buf;
}

// 从一段文本里抽出第一个可解析的 JSON 数组。优先整段解析 (STRICT JSON 时); 退化到首 '[' 到末 ']'
// 的子串 (模型偶尔包了散文/markdown 围栏)。失败返回 nullopt。
std::optional<json> extract_json_array(const std::string& text) {
    // 1) 整段就是数组?
    try {
        json j = json::parse(text);
        if (j.is_array()) return j;
    } catch (...) {
    }
    // 2) 首 '[' 到末 ']' 子串。
    const auto a = text.find('[');
    const auto b = text.rfind(']');
    if (a == std::string::npos || b == std::string::npos || b <= a) return std::nullopt;
    try {
        json j = json::parse(text.substr(a, b - a + 1));
        if (j.is_array()) return j;
    } catch (...) {
    }
    return std::nullopt;
}

}  // namespace

// ---------------------------------------------------------------------------
// 纯函数
// ---------------------------------------------------------------------------

std::vector<Candidate> filter_makeable(const std::vector<Candidate>& cands, double lo, double hi) {
    std::vector<Candidate> out;
    out.reserve(cands.size());
    for (const auto& c : cands) {
        if (c.condition_id.empty()) continue;
        if (c.mid >= lo && c.mid <= hi) out.push_back(c);
    }
    return out;
}

std::string build_prompt(const std::vector<Candidate>& cands) {
    std::string p;
    p.reserve(2048 + cands.size() * 96);
    p +=
        "You are the pool-curation layer of an automated Polymarket liquidity-rewards market maker. "
        "Each cycle I scan reward pools and must decide which ones are safe to passively quote both "
        "sides on. Your job: APPROVE the markets that are broad-based behavioral-surplus pools and "
        "REJECT the toxic ones.\n\n"
        "Reasoning axis (from market-making research):\n"
        "- GOOD (approve): broad-based, audience-driven retail flow where many uninformed takers "
        "overbet and cross-subsidize the maker. Team sports, tournaments, league/championship "
        "outrights, season win totals, reality-TV / awards / audience-vote shows. These have a "
        "makeable mid (ideally near 0.5), no single insider edge, and no imminent hard catalyst — "
        "the spread earns rewards while fills are roughly two-sided noise.\n"
        "- TOXIC (reject): single-name news, politics, geopolitics, legal/regulatory (trials, "
        "indictments, verdicts, rulings, elections, wars, coups, central-bank/Fed decisions), "
        "player transfers / trade rumors / signings, M&A or earnings, and ANYTHING insider-prone or "
        "with an imminent hard catalyst (a scheduled announcement, court date, or release that can "
        "gap the price). On these, informed flow runs one direction and adverse-selects the maker. "
        "Also reject near-extreme mids (a near-certain outcome is not makeable).\n\n"
        "Be strict: when unsure whether a market is single-name news or a broad sports/entertainment "
        "pool, REJECT it. Approving a toxic pool costs real money; skipping a good one only forgoes "
        "a little reward.\n\n"
        "Candidate pools (id | mid | question):\n";
    for (const auto& c : cands) {
        p += "- ";
        p += c.condition_id;
        p += " | mid=";
        p += fmt_mid(c.mid);
        p += " | ";
        p += c.question;
        p += "\n";
    }
    p +=
        "\nReturn ONLY a STRICT JSON array (no prose, no markdown fences) of the `id` strings you "
        "APPROVE — the condition_id values exactly as shown above. Approve nothing? Return []. "
        "Example: [\"0xabc...\", \"0xdef...\"]";
    return p;
}

json build_request_body(const std::vector<Candidate>& cands, const CurationConfig& cfg) {
    return json{
        {"model", cfg.model},
        {"max_tokens", cfg.max_tokens},
        {"messages", json::array({json{{"role", "user"}, {"content", build_prompt(cands)}}})},
    };
}

std::optional<std::set<std::string>> parse_response(const std::string& response_body,
                                                    const std::vector<Candidate>& cands) {
    json root;
    try {
        root = json::parse(response_body);
    } catch (...) {
        return std::nullopt;  // 非 JSON / 截断 → 视为失败 (保留 last-good)
    }
    if (!root.is_object()) return std::nullopt;
    // API 错误 (Anthropic: {"type":"error","error":{...}}) → 失败。
    if (const json* t = ju::find(root, "type")) {
        if (ju::to_str(*t) == "error") return std::nullopt;
    }
    // content[0].text — 取第一个 type=="text" 块的文本。
    const json* content = ju::find(root, "content");
    if (content == nullptr || !content->is_array()) return std::nullopt;
    std::string text;
    bool found_text = false;
    for (const auto& block : *content) {
        const json* bt = ju::find(block, "type");
        if (bt != nullptr && ju::to_str(*bt) == "text") {
            if (const json* tv = ju::find(block, "text")) {
                text = ju::to_str(*tv);
                found_text = true;
                break;
            }
        }
    }
    if (!found_text) return std::nullopt;

    const auto arr = extract_json_array(text);
    if (!arr) return std::nullopt;  // 文本里没有可解析数组 → 失败 (保留 last-good)

    // 已知候选 condition_id 集合: 与返回值取交集 (丢弃任何幻觉/串改的 id, 鲁棒)。
    std::set<std::string> valid;
    for (const auto& c : cands) valid.insert(c.condition_id);

    std::set<std::string> approved;
    for (const auto& v : *arr) {
        if (!v.is_string()) continue;
        const std::string id = v.get<std::string>();
        if (valid.count(id) != 0) approved.insert(id);
    }
    return approved;  // 有效数组 (哪怕交集为空) → 有效决策
}

// ---------------------------------------------------------------------------
// Curator
// ---------------------------------------------------------------------------

Curator::Curator(CurationConfig cfg) : cfg_(std::move(cfg)) {}

Curator::~Curator() {
    if (curl_ != nullptr) curl_easy_cleanup(static_cast<CURL*>(curl_));
}

std::optional<std::string> Curator::http_post(const std::string& body) {
    std::lock_guard<std::mutex> lk(curl_mu_);
    CURL* c = static_cast<CURL*>(curl_);
    if (c == nullptr) {
        c = curl_easy_init();
        curl_ = c;
    }
    if (c == nullptr) return std::nullopt;
    curl_easy_reset(c);  // 重置选项保留连接池 (keep-alive)

    // 头: x-api-key / anthropic-version / content-type。api_key 只进 header, 绝不打日志。
    curl_slist* hdr = nullptr;
    const std::string key_hdr = "x-api-key: " + cfg_.api_key;
    hdr = curl_slist_append(hdr, key_hdr.c_str());
    hdr = curl_slist_append(hdr, "anthropic-version: 2023-06-01");
    hdr = curl_slist_append(hdr, "content-type: application/json");

    std::string resp;
    curl_easy_setopt(c, CURLOPT_URL, cfg_.endpoint.c_str());
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, cfg_.timeout_s);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, cfg_.connect_timeout_s);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);

    const CURLcode rc = curl_easy_perform(c);
    long http_code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(hdr);

    if (rc != CURLE_OK) return std::nullopt;            // 网络/超时 → 失败
    if (http_code < 200 || http_code >= 300) return std::nullopt;  // 4xx/5xx → 失败
    return resp;
}

std::set<std::string> Curator::ingest_response_body(const std::string& body,
                                                    const std::vector<Candidate>& cands) {
    const auto parsed = parse_response(body, cands);
    if (parsed) {  // 有效决策 → 缓存为 last-good
        last_good_ = *parsed;
        had_success_ = true;
    }
    // 失败 (nullopt): 不动 last_good_ → 返回上一份好结果 (绝不因瞬时失败清空)。
    return last_good_;
}

std::set<std::string> Curator::curate(const std::vector<Candidate>& cands) {
    if (cfg_.api_key.empty()) return {};  // 无 key → 空 (回退静态白名单/数字滤网)
    const auto filtered = filter_makeable(cands, cfg_.mid_lo, cfg_.mid_hi);
    if (filtered.empty()) return last_good_;  // 无可做市候选 → 保留 last-good
    const std::string body = build_request_body(filtered, cfg_).dump();
    const auto resp = http_post(body);
    if (!resp) return last_good_;  // 网络/HTTP 失败 → last-good
    return ingest_response_body(*resp, filtered);
}

// ---------------------------------------------------------------------------
// 一次性无状态入口 (规格签名)
// ---------------------------------------------------------------------------

std::set<std::string> curate(const std::vector<Candidate>& cands, const CurationConfig& cfg) {
    if (cfg.api_key.empty()) return {};
    Curator c(cfg);
    return c.curate(cands);
}

}  // namespace pmm::curation
