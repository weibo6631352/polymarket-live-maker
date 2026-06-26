// apps/ws_smoke_main.cpp — WS 纯消息处理自检 (book 维护 / price_change 增量 / trade 去重)。离线。
#include "pmm/ws.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>

#include "pmm/net/ws_connection.hpp"

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}
bool near(double a, double b) { return std::abs(a - b) < 1e-9; }
}  // namespace

int main() {
    // ---- MarketChannel ----
    pmm::ws::MarketChannel mc;
    mc.set_tokens({"T1"});

    // 全量 book 快照
    mc.handle_message(
        R"({"event_type":"book","asset_id":"T1",
            "bids":[{"price":"0.49","size":"100"},{"price":"0.48","size":"50"}],
            "asks":[{"price":"0.51","size":"200"}]})");
    auto b = mc.get_book("T1");
    Check(b.has_value() && b->bids.size() == 2 && b->asks.size() == 1, "book snapshot -> 2 bids 1 ask");
    Check(near(mc.get_midpoint("T1"), 0.50), "midpoint = (0.49+0.51)/2 = 0.50");
    Check(mc.is_live(), "is_live after a frame");
    Check(mc.fresh("T1"), "token fresh after snapshot");
    Check(mc.updates("T1") == 1, "updates counter = 1");

    // price_change: 加一档 bid 0.50 → best_bid 上移
    mc.handle_message(
        R"({"event_type":"price_change","price_changes":[{"asset_id":"T1","side":"BUY","price":"0.50","size":"300"}]})");
    Check(near(mc.get_midpoint("T1"), 0.505), "after delta best_bid 0.50 -> mid 0.505");

    // price_change size 0 → 删档, best_bid 回 0.49
    mc.handle_message(
        R"({"event_type":"price_change","price_changes":[{"asset_id":"T1","side":"BUY","price":"0.50","size":"0"}]})");
    Check(near(mc.get_midpoint("T1"), 0.50), "size 0 removes level -> mid back to 0.50");

    // subscribe_msg 形态
    const std::string sub = pmm::ws::MarketChannel::subscribe_msg({"A", "B"});
    Check(sub.find("\"assets_ids\"") != std::string::npos && sub.find("\"type\":\"market\"") != std::string::npos,
          "market subscribe_msg shape");

    // apply_rest_snapshot 覆盖
    const auto rest = nlohmann::json::parse(R"({"bids":[{"price":"0.40","size":"10"}],"asks":[{"price":"0.60","size":"10"}]})");
    mc.apply_rest_snapshot("T1", rest);
    Check(near(mc.get_midpoint("T1"), 0.50), "rest snapshot overwrite -> mid (0.40+0.60)/2");
    // 单边 REST 快照被忽略
    mc.apply_rest_snapshot("T1", nlohmann::json::parse(R"({"bids":[{"price":"0.99","size":"1"}],"asks":[]})"));
    Check(near(mc.get_midpoint("T1"), 0.50), "one-sided rest snapshot ignored");

    // set_tokens 丢弃不再需要的 book
    mc.set_tokens({"T2"});
    Check(!mc.get_book("T1").has_value(), "set_tokens drops unwanted T1 book");

    // ---- UserChannel ----
    pmm::ws::UserChannel uc({"k", "s", "p"}, /*invert=*/false);
    const std::string trade = R"({"event_type":"trade","id":"tr1","asset_id":"T1","side":"BUY","size":"100","price":"0.50"})";
    uc.handle_message(trade);
    uc.handle_message(trade);  // 重复 id → 去重
    auto fills = uc.poll_fills();
    Check(fills.size() == 1, "user trade dedup -> 1 fill");
    Check(fills[0]["id"] == "tr1" && fills[0]["side"] == "BUY" && near(fills[0]["price"].get<double>(), 0.50),
          "fill fields {id,side,price}");
    Check(uc.poll_fills().empty(), "poll_fills drains queue");

    // invert_side
    pmm::ws::UserChannel uci({"k", "s", "p"}, /*invert=*/true);
    uci.handle_message(R"({"event_type":"trade","id":"tr2","asset_id":"T1","side":"BUY","size":"5","price":"0.5"})");
    auto fi = uci.poll_fills();
    Check(fi.size() == 1 && fi[0]["side"] == "SELL", "invert_side BUY -> SELL");

    const std::string usub = pmm::ws::UserChannel::subscribe_msg({"k", "s", "p"}, {"0xc"});
    Check(usub.find("\"type\":\"user\"") != std::string::npos && usub.find("\"apiKey\":\"k\"") != std::string::npos,
          "user subscribe_msg shape (auth)");

    // ---- WS 帧编码 (掩码 + 长度) 往返 ----
    {
        const std::string payload = "hello-ws-\xe4\xb8\x96\xe7\x95\x8c";  // 含多字节 (世界)
        const auto frame = pmm::net::WsConnection::encode_text_frame(payload);
        Check(frame.size() >= 2 && frame[0] == 0x81, "frame byte0 = FIN+text(0x81)");
        Check((frame[1] & 0x80) != 0, "frame MASK bit set (client->server)");
        const std::size_t plen = payload.size();
        Check(plen <= 125 && (frame[1] & 0x7F) == plen, "frame short length encoded");
        const std::uint8_t* mask = &frame[2];
        std::string dec;
        for (std::size_t i = 0; i < plen; ++i)
            dec.push_back(static_cast<char>(frame[6 + i] ^ mask[i % 4]));
        Check(dec == payload, "frame unmask round-trip recovers payload");

        // 长 payload (>125, <=65535) → 126 扩展长度
        const std::string big(300, 'x');
        const auto bf = pmm::net::WsConnection::encode_text_frame(big);
        Check((bf[1] & 0x7F) == 126 && ((static_cast<int>(bf[2]) << 8) | bf[3]) == 300,
              "frame 16-bit extended length = 300");
    }

    // ---- 评审修复回归: UserChannel seen_ 改真 FIFO, 重发"最新"id 必去重 ----
    {
        pmm::ws::UserChannel u({"k", "s", "p"}, false);
        for (int i = 0; i <= 5000; ++i) {  // 5001 个不同 trade → 触发上限回收
            u.handle_message(std::string("{\"event_type\":\"trade\",\"id\":\"tr") + std::to_string(i) +
                             "\",\"asset_id\":\"T\",\"side\":\"BUY\",\"size\":\"1\",\"price\":\"0.5\"}");
        }
        (void)u.poll_fills();  // 排空队列
        // 重发最新插入的 tr5000: FIFO 保留最新 → 去重 (旧 sorted-set 会误淘汰它 → 重复入队)。
        u.handle_message(R"({"event_type":"trade","id":"tr5000","asset_id":"T","side":"BUY","size":"1","price":"0.5"})");
        Check(u.poll_fills().empty(), "review-fix: seen FIFO keeps newest id deduped after recycle");
    }

    std::printf(g_fail ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
