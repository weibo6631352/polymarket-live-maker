// pmm/curation.hpp — 自主 LLM 选池 (Anthropic Claude API 精选宽基行为盈余池)
//
// 静态语义白名单脆且需人工维护。本模块每个发现周期 (~10min) 把当前候选池送给 Claude, 让它精选
// "宽基行为盈余" 池 (体育/锦标赛/真人秀 — 散户高估的零售流喂做市方), 拒绝单名新闻/政治/法律/转会
// (内幕+硬催化剂=毒)。返回的批准集合作动态白名单合并进 select_pools (白名单绕过脆的数字滤网)。
//
// 鲁棒性 (关键 — 遥测/选池绝不能搞挂交易): 无 key → 返回空 (回退静态白名单/数字滤网, 行为不变);
// API 失败/超时/垃圾 → 返回上一份好结果 (缓存, 绝不因瞬时失败清空白名单)。跑在发现节奏, 非热路径。
//
// API key 来自 env LM_ANTHROPIC_KEY (运行期机密, 同 PM 私钥 — 绝不硬编码或打日志)。
#pragma once

#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace pmm::curation {

// 一个候选池 (送给 LLM 评判的最小信息)。
struct Candidate {
    std::string question;
    std::string condition_id;
    double mid{0.0};
};

// 选池调用配置。api_key 是运行期机密 (绝不打日志); 其余可从 env/默认派生。
struct CurationConfig {
    std::string api_key;                                       // LM_ANTHROPIC_KEY (机密, 绝不日志)
    std::string model{"claude-sonnet-4-6"};                    // LM_CURATION_MODEL
    std::string endpoint{"https://api.anthropic.com/v1/messages"};
    int max_tokens{4096};
    long timeout_s{15};         // 整体超时 (~15s; 发现节奏可容忍)
    long connect_timeout_s{8};
    double mid_lo{0.15};        // 只送 mid∈[lo,hi] 的可做市池 (剔近极端价, 缩小 prompt)
    double mid_hi{0.85};
};

// ---- 纯函数 (导出供单测; 不触网) ----

// 把候选裁到 mid∈[lo,hi] 的可做市区间 (剔近极端价池, 缩小 prompt)。
[[nodiscard]] std::vector<Candidate> filter_makeable(const std::vector<Candidate>& cands, double lo, double hi);

// 构造发给 Claude 的 user prompt 文本 (做市研究的完整推理轴 + 候选清单 + STRICT JSON 返回约定)。
[[nodiscard]] std::string build_prompt(const std::vector<Candidate>& cands);

// 构造 /v1/messages 请求体 {model, max_tokens, messages:[{role:user, content:prompt}]}。
[[nodiscard]] nlohmann::json build_request_body(const std::vector<Candidate>& cands, const CurationConfig& cfg);

// 解析 Claude /v1/messages 响应体 → 取 content[0].text → 抽 JSON 数组 → 与候选 condition_id 取交集。
// 返回值语义 (区分 "有效空批准" 与 "解析失败" 以支持 last-good):
//   - 成功解析出 JSON 数组 (哪怕空 []) → 返回交集集合 (可能为空) —— 这是模型的有效决策。
//   - 顶层是 API error / 无 content / 文本里找不到可解析的数组 / json 解析失败 → nullopt (调用方保留 last-good)。
[[nodiscard]] std::optional<std::set<std::string>> parse_response(const std::string& response_body,
                                                                  const std::vector<Candidate>& cands);

// 有状态精选器: 持有 last-good 缓存; curate() 走完整链路 (建 prompt → POST → 解析 → last-good)。
// 跑在发现线程 (off 热路径)。线程安全: curl 句柄串行化。
class Curator {
public:
    explicit Curator(CurationConfig cfg);
    ~Curator();
    Curator(const Curator&) = delete;
    Curator& operator=(const Curator&) = delete;

    // 完整链路: 无 key → 空; 成功 → 缓存并返回批准集; 失败/垃圾 → 返回 last-good (绝不清空)。
    std::set<std::string> curate(const std::vector<Candidate>& cands);

    // 单测/共享 seam: 应用一条原始 /v1/messages 响应体 (跳过网络)。nullopt 解析 → 保留 last-good。
    std::set<std::string> ingest_response_body(const std::string& body, const std::vector<Candidate>& cands);

    [[nodiscard]] const std::set<std::string>& last_good() const { return last_good_; }
    [[nodiscard]] bool had_success() const { return had_success_; }
    [[nodiscard]] const CurationConfig& config() const { return cfg_; }

private:
    // HTTP POST 到 Anthropic /v1/messages; 2xx 返回响应体, 否则 nullopt。绝不打 api_key。
    [[nodiscard]] std::optional<std::string> http_post(const std::string& body);

    CurationConfig cfg_;
    std::set<std::string> last_good_;
    bool had_success_{false};
    void* curl_{nullptr};      // CURL* (持久句柄, keep-alive)
    std::mutex curl_mu_;
};

// 一次性无状态入口 (规格签名): 无 key → 空; 失败/垃圾 → 空 (无跨调用缓存)。
// 运行期带 last-good 的路径用 Curator (上)。
[[nodiscard]] std::set<std::string> curate(const std::vector<Candidate>& cands, const CurationConfig& cfg);

}  // namespace pmm::curation
