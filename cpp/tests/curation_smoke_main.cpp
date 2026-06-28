// tests/curation_smoke_main.cpp — pmm::curation 自检: prompt 构造 + 响应解析 (MOCK) + 回退链。
//
// 绝不触网 (无 key): 测 (1) build_prompt 含推理轴 + 候选清单 + STRICT JSON 约定;
// (2) parse_response 从 MOCK 的 Claude 响应抽出正确 condition_ids (含幻觉 id 丢弃 / 散文包裹 / 空批准);
// (3) 回退: 空 key → curate 返回空; API error / 垃圾响应 → nullopt → Curator 保留 last-good。
#include "pmm/curation.hpp"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

using nlohmann::json;
namespace cur = pmm::curation;

// 构造一条形如 Anthropic /v1/messages 的响应 (content[0].text = 给定文本)。
std::string mock_response(const std::string& text) {
    json content = json::array();
    content.push_back(json{{"type", "text"}, {"text", text}});
    json m = {{"id", "msg_1"}, {"type", "message"}, {"role", "assistant"},
              {"content", content}, {"stop_reason", "end_turn"}};
    return m.dump();
}
}  // namespace

int main() {
    // 三个候选: 两个宽基体育 (应批准) + 一个单名法律新闻 (应拒)。
    const std::vector<cur::Candidate> cands = {
        {"Lakers vs Celtics — who wins?", "0xAAA", 0.52},
        {"Will Senator X be indicted by Friday?", "0xBBB", 0.40},
        {"Premier League winner: Arsenal?", "0xCCC", 0.30},
    };

    // ---- (1) prompt 构造 ----
    const std::string prompt = cur::build_prompt(cands);
    Check(prompt.find("APPROVE") != std::string::npos, "prompt has APPROVE axis");
    Check(prompt.find("REJECT") != std::string::npos, "prompt has REJECT axis");
    Check(prompt.find("STRICT JSON") != std::string::npos, "prompt demands STRICT JSON");
    Check(prompt.find("condition_id") != std::string::npos, "prompt asks for condition_ids");
    Check(prompt.find("0xAAA") != std::string::npos && prompt.find("0xBBB") != std::string::npos &&
              prompt.find("0xCCC") != std::string::npos,
          "prompt lists all candidate ids");
    Check(prompt.find("mid=0.52") != std::string::npos, "prompt shows candidate mid");
    Check(prompt.find("Premier League winner: Arsenal?") != std::string::npos,
          "prompt shows candidate question");

    const json body = cur::build_request_body(cands, cur::CurationConfig{});
    Check(body.value("model", std::string{}) == "claude-sonnet-4-6", "body default model");
    Check(body.contains("max_tokens"), "body has max_tokens");
    Check(body["messages"].is_array() && body["messages"].size() == 1 &&
              body["messages"][0].value("role", std::string{}) == "user",
          "body single user message");
    Check(!body["messages"][0].value("content", std::string{}).empty(), "body message content non-empty");

    // ---- (2) 响应解析 (MOCK) ----
    {  // 标准: 批准两个体育池, 拒法律池。
        const auto r = cur::parse_response(mock_response("[\"0xAAA\", \"0xCCC\"]"), cands);
        Check(r.has_value(), "parse: valid array -> value");
        Check(r && *r == std::set<std::string>{"0xAAA", "0xCCC"}, "parse: extracts 0xAAA + 0xCCC");
        Check(r && r->count("0xBBB") == 0, "parse: 0xBBB not approved");
    }
    {  // 幻觉 id (不在候选里) 被交集丢弃。
        const auto r = cur::parse_response(mock_response("[\"0xAAA\", \"0xZZZ\"]"), cands);
        Check(r && *r == std::set<std::string>{"0xAAA"}, "parse: drops hallucinated id 0xZZZ");
    }
    {  // 模型把数组包在散文里 (非严格) — 子串抽取仍能拿到。
        const auto r =
            cur::parse_response(mock_response("Approved pools:\n[\"0xCCC\"]\nDone."), cands);
        Check(r && *r == std::set<std::string>{"0xCCC"}, "parse: extracts array from prose");
    }
    {  // 有效空批准 [] → 空集 (有值, 非失败)。
        const auto r = cur::parse_response(mock_response("[]"), cands);
        Check(r.has_value() && r->empty(), "parse: valid empty [] -> empty value");
    }
    {  // API error 顶层 → nullopt (失败)。
        json err = {{"type", "error"}, {"error", {{"type", "overloaded_error"}, {"message", "x"}}}};
        const auto r = cur::parse_response(err.dump(), cands);
        Check(!r.has_value(), "parse: API error -> nullopt");
    }
    {  // 垃圾/截断响应 → nullopt (失败)。
        const auto r = cur::parse_response("not json at all {", cands);
        Check(!r.has_value(), "parse: garbage body -> nullopt");
    }
    {  // 有 content 但文本里无数组 → nullopt (失败)。
        const auto r = cur::parse_response(mock_response("I approve the sports ones."), cands);
        Check(!r.has_value(), "parse: no array in text -> nullopt");
    }

    // ---- (3a) 回退: 空 key → curate 返回空 (绝不触网) ----
    {
        cur::CurationConfig cfg;  // api_key 默认空
        const auto r = cur::curate(cands, cfg);
        Check(r.empty(), "curate: empty key -> empty (no network)");
    }

    // ---- (3b) last-good: 成功缓存; 失败/垃圾保留上一份好结果 (经 ingest_response_body seam, 不触网) ----
    {
        cur::CurationConfig cfg;
        cfg.api_key = "test-key-not-used";  // 仅构造; ingest_response_body 不发请求
        cur::Curator c(cfg);

        const auto good = c.ingest_response_body(mock_response("[\"0xAAA\", \"0xCCC\"]"), cands);
        Check(good == std::set<std::string>{"0xAAA", "0xCCC"}, "ingest: good response cached");
        Check(c.had_success(), "ingest: had_success after good");

        const auto after_garbage = c.ingest_response_body("garbage }{", cands);
        Check(after_garbage == std::set<std::string>{"0xAAA", "0xCCC"},
              "ingest: garbage keeps LAST-GOOD (not blanked)");

        const auto after_error =
            c.ingest_response_body(json{{"type", "error"}, {"error", {{"type", "x"}}}}.dump(), cands);
        Check(after_error == std::set<std::string>{"0xAAA", "0xCCC"},
              "ingest: API error keeps LAST-GOOD");

        // 有效空批准 [] 确实覆盖 last-good (模型真的批准了空 — 非失败)。
        const auto after_empty = c.ingest_response_body(mock_response("[]"), cands);
        Check(after_empty.empty(), "ingest: valid [] overwrites last-good (real decision)");
    }

    // ---- filter_makeable: 剔近极端价 ----
    {
        const std::vector<cur::Candidate> wide = {
            {"q1", "0x1", 0.05}, {"q2", "0x2", 0.50}, {"q3", "0x3", 0.95}};
        const auto f = cur::filter_makeable(wide, 0.15, 0.85);
        Check(f.size() == 1 && f[0].condition_id == "0x2", "filter_makeable keeps only mid in [0.15,0.85]");
    }

    std::printf(g_fail ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
