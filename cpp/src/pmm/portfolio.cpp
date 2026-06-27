// src/pmm/portfolio.cpp — 选池配资编排实现 (port of pm_trader/portfolio.py 的纯选择部分)
#include "pmm/portfolio.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "pmm/orderbook.hpp"
#include "pmm/round.hpp"

namespace pmm::portfolio {

namespace ob = pmm::orderbook;

namespace {

// 无相关性信号的通用词 (两市场只共享这些 ⇒ 不算同一事件)。
const std::set<std::string>& stopwords() {
    static const std::set<std::string> kSet = {
        "will", "the",  "be",   "in",     "on",    "at",    "a",     "an",    "of",    "to",
        "and",  "or",   "by",   "win",    "wins",  "won",   "most",  "next",  "for",   "vs",
        "end",  "up",   "down", "above",  "below", "before","after", "into",  "out",   "as",
        "is",   "are",  "reach","hit",    "with",  "who",   "what",  "when",  "which", "than",
        "then", "over", "under","between"};
    return kSet;
}

// 精选话题/实体簇: 命中同簇的市场一起动 (同 FOMC / 同国政治 / 同场比赛)。
const std::vector<std::pair<std::string, std::vector<std::string>>>& cluster_rules() {
    static const std::vector<std::pair<std::string, std::vector<std::string>>> kRules = {
        {"us-fed", {"fed", "fomc", "federal reserve"}},
        {"romania", {"romania", "grindeanu", "bolojan", "ciolacu"}},
        {"colombia", {"colombia", "petro"}},
        {"israel", {"israel", "netanyahu", "knesset", "eizenkot", "bennett"}},
        {"france", {"france", "french", "bardella", "macron", "philippe", "melenchon"}},
        {"ecb", {"ecb", "european central bank", "lagarde"}},
        {"boe", {"bank of england", "boe"}},
    };
    return kRules;
}

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c);
    return s;
}

bool all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

std::size_t intersection_size(const std::set<std::string>& a, const std::set<std::string>& b) {
    std::size_t n = 0;
    for (const auto& x : a) {
        if (b.count(x) != 0) ++n;
    }
    return n;
}

// _deploy_size: 资本感知的下单 size (>= min_size), 受 资本/份额/跳跃 三限。
double deploy_size(const rewards::PoolReport& p, double half_spread_c, double target_cap,
                   double share_cap, double loss_budget) {
    const double min_size = p.min_size;
    const double per_share = ob::committed_capital(min_size, half_spread_c) / min_size;
    const double cap_capital = per_share > 0.0 ? (target_cap / per_share) : min_size;
    const double w = ob::maker_quote_score(1.0, half_spread_c, p.max_spread_c);
    const double comp = p.min_side_score;  // 竞争者 Qmin (PoolReport 总有值; None→0 等价)
    const double cap_share =
        (w > 0.0 && share_cap > 0.0 && share_cap < 1.0)
            ? (share_cap / (1.0 - share_cap) * comp / w)
            : cap_capital;
    const double mj = p.max_jump_c.value_or(0.0) / 100.0;
    const double cap_risk = mj > 0.0 ? (loss_budget / mj) : cap_capital;
    return std::max(min_size, std::min({cap_capital, cap_share, cap_risk}));
}

}  // namespace

std::set<std::string> significant_tokens(const std::string& question) {
    const std::string q = to_lower(question);
    std::set<std::string> out;
    std::string cur;
    auto flush = [&] {
        if (cur.size() > 2 && !all_digits(cur) && stopwords().count(cur) == 0) out.insert(cur);
        cur.clear();
    };
    for (char c : q) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            cur.push_back(c);
        } else {
            flush();
        }
    }
    flush();
    return out;
}

std::string cluster_key(const std::string& question) {
    const std::string q = to_lower(question);
    for (const auto& [name, keywords] : cluster_rules()) {
        for (const auto& kw : keywords) {
            if (q.find(kw) != std::string::npos) return name;
        }
    }
    return {};
}

double risk_adjusted_score(const rewards::PoolReport& p, double risk_tolerance_days,
                           double chop_aversion, double compet_aversion) {
    const double reward = p.reward_per_day;  // PoolReport 总有 reward_per_day (= share*daily, 已 round)
    const double days_wiped = p.days_wiped.value_or(9999.0);  // None(无历史) → 9999
    const double jump_disc = 1.0 + days_wiped / risk_tolerance_days;
    const double vol = p.daily_vol_c.value_or(0.0);  // 0/None → 无 chop 惩罚
    const double band = p.max_spread_c;
    const double chop_disc = 1.0 + chop_aversion * (band > 0.0 ? (vol / band) : 0.0);
    // 竞争度惩罚: PM 官方 market_competitiveness (实测 0.46–4.2, 越高越挤 = 我们份额被稀释越狠)。
    // 拥挤池降权 → 偏好清静池 (实测好选 0x551db8 comp0.47, 烂选 0xdf2020 comp4.2 小池)。<0=未知, 不罚。
    const double comp_disc =
        (compet_aversion > 0.0 && p.competitiveness >= 0.0) ? (1.0 + compet_aversion * p.competitiveness) : 1.0;
    return reward / (jump_disc * chop_disc * comp_disc);
}

std::vector<SelectedPool> select_pools(const rewards::ScanResult& scan_report,
                                       const SelectParams& params) {
    const auto& cd = params.cooldown;

    // 1. 资格过滤 (cooldown + SAFE/non-empty-band)
    std::vector<const rewards::PoolReport*> cands;
    for (const auto& p : scan_report.pools) {
        if (cd.count(p.condition_id) != 0 || cd.count(p.token) != 0) continue;
        if (params.require_safe && !(p.jump_verdict == "SAFE" && !p.empty_band)) continue;
        cands.push_back(&p);
    }

    // 2. 按风险调整收益排序 (稳定, 降序)
    std::vector<std::pair<const rewards::PoolReport*, double>> scored;
    scored.reserve(cands.size());
    for (const auto* p : cands) {
        scored.push_back({p, risk_adjusted_score(*p, params.risk_tolerance_days, params.chop_aversion,
                                                  params.compet_aversion)});
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });

    // 3. 动态质量地板 (不是硬池数): 保留 >= quality_floor_frac * 最优分 的池
    const double best = scored.empty() ? 0.0 : scored.front().second;
    const double cutoff = params.quality_floor_frac > 0.0 ? params.quality_floor_frac * best : 0.0;
    std::vector<std::pair<const rewards::PoolReport*, double>> elig;
    for (const auto& ps : scored) {
        if (ps.second >= cutoff) elig.push_back(ps);
    }
    double total_score = 0.0;
    for (const auto& ps : elig) total_score += ps.second;
    if (total_score == 0.0) total_score = 1.0;

    // 4*. 注水配资 (waterfill): 先按分数+去相关选池、每池下 min_size, 再把剩余资本按"边际 κ×奖励/美元"
    //     注水。奖励份额对 size 是凹的 (share=ours/(ours+E) 饱和) → 边际递减 → 注水均衡边际 = 最大化组合奖励。
    //     取代"capital ∝ score"(无视凹性, 会过度配资到已饱和的高分池)。+7.9% (量化备忘), 现按校准净值。
    if (params.waterfill) {
        const double infl = 1.0 / std::max(params.reward_calib, 1e-6);  // 竞争充气 = 校准真实份额
        struct WF {
            const rewards::PoolReport* p;
            double hs, per_share, size, cap, max_size, score;
        };
        std::vector<WF> wf;
        std::vector<std::set<std::string>> chosen_tokens;
        std::set<std::string> chosen_clusters;
        double spent = 0.0;
        for (const auto& [p, score] : elig) {  // 4a. 选池 (分数序, 去相关) + min_size 起步
            const double hs = p->tick * 100.0 * params.half_spread_ticks;
            const double per_share = ob::committed_capital(p->min_size, hs) / p->min_size;
            const double min_cap = ob::committed_capital(p->min_size, hs);
            if (min_cap <= 0.0 || spent + min_cap > params.capital) continue;
            const std::string cluster = cluster_key(p->question);
            if (!cluster.empty() && chosen_clusters.count(cluster) != 0) continue;
            const std::set<std::string> toks = significant_tokens(p->question);
            bool corr = false;
            for (const auto& ct : chosen_tokens)
                if (static_cast<long>(intersection_size(toks, ct)) > params.max_token_overlap) {
                    corr = true;
                    break;
                }
            if (corr) continue;
            const double rp = params.loss_budget > 0.0 ? params.loss_budget / static_cast<double>(elig.size()) : 1e18;
            const double max_size = std::max(deploy_size(*p, hs, params.capital, params.size_share_cap, rp), p->min_size);
            wf.push_back({p, hs, per_share, p->min_size, min_cap, max_size, score});
            spent += min_cap;
            chosen_tokens.push_back(toks);
            if (!cluster.empty()) chosen_clusters.insert(cluster);
        }
        const double chunk = std::max(1.0, params.capital * 0.005);  // 4b. 注水剩余资本
        auto marg = [&](const WF& a) -> double {  // 边际 κ×奖励/美元 (再投 ~chunk 的奖励增量/资本增量)
            if (a.size >= a.max_size - 1e-9 || a.per_share <= 1e-9) return 0.0;
            const double dsize = std::min(chunk / a.per_share, a.max_size - a.size);
            const double s0 = ob::maker_reward_share(a.size, a.hs, a.p->max_spread_c, a.p->min_side_score * infl);
            const double s1 = ob::maker_reward_share(a.size + dsize, a.hs, a.p->max_spread_c, a.p->min_side_score * infl);
            return (s1 - s0) * a.p->daily / (dsize * a.per_share);
        };
        while (params.capital - spent > chunk && !wf.empty()) {
            int bi = -1;
            double bm = 0.0;
            for (std::size_t i = 0; i < wf.size(); ++i) {
                const double m = marg(wf[i]);
                if (m > bm) {
                    bm = m;
                    bi = static_cast<int>(i);
                }
            }
            if (bi < 0) break;  // 全部到顶
            WF& a = wf[static_cast<std::size_t>(bi)];
            const double dsize = std::min(chunk / a.per_share, a.max_size - a.size);
            const double dcap = dsize * a.per_share;
            if (dcap <= 1e-9) break;
            a.size += dsize;
            a.cap += dcap;
            spent += dcap;
        }
        std::vector<SelectedPool> out;  // 4c. 构建
        for (const auto& a : wf) {
            const double share = ob::maker_reward_share(a.size, a.hs, a.p->max_spread_c, a.p->min_side_score * infl);
            SelectedPool sp;
            sp.question = a.p->question;
            sp.condition_id = a.p->condition_id;
            sp.token = a.p->token;
            sp.daily = a.p->daily;
            sp.share = round_to(share, 4);
            sp.min_size = a.p->min_size;
            sp.size = round_to(a.size, 2);
            sp.tick = a.p->tick;
            sp.max_spread_c = a.p->max_spread_c;
            sp.half_spread_c = a.hs;
            sp.committed_capital = round_to(a.cap, 2);
            sp.est_daily_reward = round_to(share * a.p->daily, 4);  // 校准份额 × daily
            sp.risk_adj_score = round_to(a.score, 4);
            out.push_back(std::move(sp));
        }
        return out;
    }

    // 4. 贪心配资 (相关性/簇去重 + 预算)
    std::vector<SelectedPool> selected;
    std::vector<std::set<std::string>> chosen_tokens;
    std::set<std::string> chosen_clusters;
    double spent = 0.0;

    for (const auto& [p, score] : elig) {
        const double half_spread_c = p->tick * 100.0 * params.half_spread_ticks;
        // 始终按质量加权配资 (capital ∝ score, max_pool_frac 封顶 + deploy_size 的份额/跳变上限);
        // 预算紧/loss_budget=0 时自动退化到 min_size。池数由质量地板+预算治理, 无硬池数上限。
        const double weight = score / total_score;
        const double target = std::min(params.max_pool_frac * params.capital, params.capital * weight);
        const double rp = params.loss_budget > 0.0 ? params.loss_budget * weight : 0.0;
        const double size = deploy_size(*p, half_spread_c, target, params.size_share_cap, rp);
        const double share = ob::maker_reward_share(size, half_spread_c, p->max_spread_c, p->min_side_score);
        const double cap = ob::committed_capital(size, half_spread_c);
        if (cap <= 0.0 || spent + cap > params.capital) continue;

        const std::string cluster = cluster_key(p->question);
        if (!cluster.empty() && chosen_clusters.count(cluster) != 0) continue;  // 同簇 ⇒ 相关

        const std::set<std::string> toks = significant_tokens(p->question);
        if (!toks.empty()) {
            bool correlated = false;
            for (const auto& ct : chosen_tokens) {
                // 有符号比较 (对齐 Python len(...) > max_token_overlap); 负的 max_token_overlap 才不会回绕。
                if (static_cast<long>(intersection_size(toks, ct)) > params.max_token_overlap) {
                    correlated = true;
                    break;
                }
            }
            if (correlated) continue;
        }

        SelectedPool sp;
        sp.question = p->question;
        sp.condition_id = p->condition_id;
        sp.token = p->token;
        sp.daily = p->daily;
        sp.share = round_to(share, 4);
        sp.min_size = p->min_size;
        sp.size = round_to(size, 2);
        sp.tick = p->tick;
        sp.max_spread_c = p->max_spread_c;
        sp.half_spread_c = half_spread_c;
        sp.committed_capital = round_to(cap, 2);
        sp.est_daily_reward = round_to(share * p->daily, 4);
        sp.risk_adj_score = round_to(score, 4);
        selected.push_back(std::move(sp));

        spent += cap;
        chosen_tokens.push_back(toks);
        if (!cluster.empty()) chosen_clusters.insert(cluster);
    }
    return selected;
}

}  // namespace pmm::portfolio
