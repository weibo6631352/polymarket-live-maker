// pmm/net/ws_connection.hpp — 最小 WSS 客户端 (TLS + RFC6455 帧)。单线程使用 (每频道一个读线程独占)。
//
// 移植自 sports-trader-cpp live_wss_transport 的握手/帧编解码 + persistent_https 的 TLS 连接风格。
// pm 用法 (ws.py): 连接 → 发订阅 → 循环 recv; 每 10s 发文本 "PING" (非控制帧); 出错关连接重连。
#pragma once

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace pmm::net {

struct WsMessage {
    enum Kind { Text, Timeout, Closed };
    Kind kind{Timeout};
    std::string text;
};

class WsConnection {
public:
    WsConnection(std::string host, std::string path, int timeout_ms = 2000) noexcept
        : host_(std::move(host)), path_(std::move(path)), timeout_ms_(timeout_ms) {}
    ~WsConnection() { close(); }
    WsConnection(const WsConnection&) = delete;
    WsConnection& operator=(const WsConnection&) = delete;

    [[nodiscard]] bool connected() const noexcept { return ssl_ != nullptr; }

    // 从另一线程中断阻塞的 reader (::shutdown 让 SSL_read 立即出错 → recv 返回 Closed)。fd 不关 (close() 才关)。
    void shutdown_socket() noexcept {
        const int fd = fd_;
        if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
    }

    // TCP + TLS + RFC6455 升级握手。成功返回 true。
    [[nodiscard]] bool connect() noexcept {
        close();
        if (!open_tls()) {
            close();
            return false;
        }
        if (!handshake()) {
            close();
            return false;
        }
        return true;
    }

    // 发掩码文本帧 (客户端→服务端必须 mask, RFC6455 §5.3)。
    [[nodiscard]] bool send_text(const std::string& payload) noexcept {
        if (ssl_ == nullptr) return false;
        const std::vector<std::uint8_t> frame = encode_text_frame(payload);
        return ssl_write_all(frame.data(), frame.size());
    }

    // 收一帧。控制帧 (ping→回 pong, pong/close) 内部处理。Text 返回文本; Timeout=暂无数据; Closed=断/错。
    WsMessage recv() noexcept {
        if (ssl_ == nullptr) return {WsMessage::Closed, {}};
        std::uint8_t hdr0 = 0;
        const int r = SSL_read(ssl_, &hdr0, 1);  // 第一字节: 超时则返回 Timeout (让上层发 PING)
        if (r <= 0) return is_timeout(r) ? WsMessage{WsMessage::Timeout, {}} : WsMessage{WsMessage::Closed, {}};
        std::uint8_t hdr1 = 0;
        if (!read_exact(&hdr1, 1)) return {WsMessage::Closed, {}};

        const bool fin = (hdr0 & 0x80) != 0;
        const std::uint8_t opcode = hdr0 & 0x0F;
        const bool masked = (hdr1 & 0x80) != 0;
        std::uint64_t len = hdr1 & 0x7F;
        if (len == 126) {
            std::uint8_t e[2];
            if (!read_exact(e, 2)) return {WsMessage::Closed, {}};
            len = (static_cast<std::uint64_t>(e[0]) << 8) | e[1];
        } else if (len == 127) {
            std::uint8_t e[8];
            if (!read_exact(e, 8)) return {WsMessage::Closed, {}};
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | e[static_cast<std::size_t>(i)];
        }
        std::uint8_t mask[4] = {0, 0, 0, 0};
        if (masked && !read_exact(mask, 4)) return {WsMessage::Closed, {}};

        static constexpr std::uint64_t kMaxPayload = 2 * 1024 * 1024;  // 2MB 上限 (防 OOM/恶意帧)
        if (len > kMaxPayload) {
            std::uint64_t remaining = len;  // drain 保持帧同步
            std::uint8_t buf[4096];
            while (remaining > 0) {
                const std::size_t chunk = remaining > sizeof(buf) ? sizeof(buf)
                                                                  : static_cast<std::size_t>(remaining);
                if (!read_exact(buf, chunk)) return {WsMessage::Closed, {}};
                remaining -= chunk;
            }
            return {WsMessage::Timeout, {}};  // 丢弃, 当作无消息
        }

        std::vector<std::uint8_t> payload(len);
        if (len > 0 && !read_exact(payload.data(), len)) return {WsMessage::Closed, {}};
        if (masked) {
            for (std::size_t i = 0; i < len; ++i) payload[i] ^= mask[i % 4];
        }

        if (opcode == 0x8) return {WsMessage::Closed, {}};   // close
        if (opcode == 0x9) {                                  // ping → pong
            send_pong(payload);
            return {WsMessage::Timeout, {}};
        }
        if (opcode == 0xA) return {WsMessage::Timeout, {}};  // pong, 忽略

        if (opcode == 0x1 || opcode == 0x2 || opcode == 0x0) {  // text/binary/continuation
            if (opcode != 0x0) frag_.clear();  // 0x1/0x2 = 新消息起始 → 丢弃上条未完分片 (RFC6455)
            frag_.insert(frag_.end(), payload.begin(), payload.end());
            if (fin) {
                std::string text(reinterpret_cast<const char*>(frag_.data()), frag_.size());
                frag_.clear();
                return {WsMessage::Text, std::move(text)};
            }
            return {WsMessage::Timeout, {}};  // 分片未完
        }
        return {WsMessage::Timeout, {}};
    }

    void close() noexcept {
        if (ssl_ != nullptr) {
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
        if (ctx_ != nullptr) {
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        frag_.clear();
    }

    // 掩码文本帧编码 (静态, 可离线单测)。
    [[nodiscard]] static std::vector<std::uint8_t> encode_text_frame(const std::string& payload) {
        const std::size_t plen = payload.size();
        std::vector<std::uint8_t> frame;
        frame.reserve(plen + 14);
        frame.push_back(0x81);  // FIN + text(0x1)
        if (plen <= 125) {
            frame.push_back(static_cast<std::uint8_t>(0x80 | plen));
        } else if (plen <= 65535) {
            frame.push_back(0xFE);  // 0x80 | 126
            frame.push_back(static_cast<std::uint8_t>((plen >> 8) & 0xFF));
            frame.push_back(static_cast<std::uint8_t>(plen & 0xFF));
        } else {
            frame.push_back(0xFF);  // 0x80 | 127
            for (int i = 7; i >= 0; --i) frame.push_back(static_cast<std::uint8_t>((plen >> (8 * i)) & 0xFF));
        }
        std::uint8_t mask[4];
        if (RAND_bytes(mask, sizeof(mask)) != 1) {
            for (std::size_t i = 0; i < 4; ++i) mask[i] = static_cast<std::uint8_t>(0x5a + i);
        }
        frame.insert(frame.end(), mask, mask + 4);
        for (std::size_t i = 0; i < plen; ++i) {
            frame.push_back(static_cast<std::uint8_t>(static_cast<std::uint8_t>(payload[i]) ^ mask[i % 4]));
        }
        return frame;
    }

private:
    [[nodiscard]] bool is_timeout(int ret) const noexcept {
        const int err = SSL_get_error(ssl_, ret);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) return true;
        return err == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
    }

    // 阻塞读满 n (帧中途的超时有界重试; 连续超时过多视为断)。
    [[nodiscard]] bool read_exact(void* buf, std::size_t n) noexcept {
        std::size_t done = 0;
        auto* p = static_cast<std::uint8_t*>(buf);
        int timeouts = 0;
        while (done < n) {
            const int r = SSL_read(ssl_, p + done, static_cast<int>(n - done));
            if (r <= 0) {
                if (is_timeout(r) && ++timeouts < 15) continue;  // 帧中途短暂超时 → 重试 (上限 ~30s)
                return false;
            }
            done += static_cast<std::size_t>(r);
        }
        return true;
    }

    [[nodiscard]] bool ssl_write_all(const std::uint8_t* data, std::size_t n) noexcept {
        std::size_t done = 0;
        int timeouts = 0;
        while (done < n) {
            const int r = SSL_write(ssl_, data + done, static_cast<int>(n - done));
            if (r <= 0) {
                if (is_timeout(r) && ++timeouts < 15) continue;  // 半死对端: 有界重试 (~30s), 防永久 spin
                return false;
            }
            done += static_cast<std::size_t>(r);
        }
        return true;
    }

    void send_pong(const std::vector<std::uint8_t>& ping_payload) noexcept {
        std::vector<std::uint8_t> pong;
        pong.push_back(0x8A);  // FIN + pong(0xA)
        pong.push_back(static_cast<std::uint8_t>(ping_payload.size() & 0x7F));
        pong.insert(pong.end(), ping_payload.begin(), ping_payload.end());
        (void)ssl_write_all(pong.data(), pong.size());
    }

    // TCP + TLS (同 persistent_https: 非阻塞 connect + poll 超时, TLS1.2+ 验证 + SNI/hostname)。
    [[nodiscard]] bool open_tls() noexcept {
        struct addrinfo hints{};
        struct addrinfo* res = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host_.c_str(), "443", &hints, &res) != 0 || res == nullptr) return false;
        fd_ = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd_ < 0) {
            freeaddrinfo(res);
            return false;
        }
        struct timeval tv{};
        tv.tv_sec = timeout_ms_ / 1000;
        tv.tv_usec = (timeout_ms_ % 1000) * 1000;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        const int fl = ::fcntl(fd_, F_GETFL, 0);
        ::fcntl(fd_, F_SETFL, fl | O_NONBLOCK);
        const int crc = ::connect(fd_, res->ai_addr, res->ai_addrlen);
        if (crc != 0 && errno != EINPROGRESS) {
            freeaddrinfo(res);
            return false;
        }
        if (crc != 0) {
            struct pollfd pfd{};
            pfd.fd = fd_;
            pfd.events = POLLOUT;
            const int pr = ::poll(&pfd, 1, timeout_ms_);
            int soerr = 0;
            socklen_t sl = sizeof(soerr);
            if (pr <= 0 || ::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &soerr, &sl) != 0 || soerr != 0) {
                freeaddrinfo(res);
                return false;
            }
        }
        ::fcntl(fd_, F_SETFL, fl);  // 恢复阻塞 (后续 SSL 用阻塞 + SO_*TIMEO)
        freeaddrinfo(res);
        ctx_ = SSL_CTX_new(TLS_client_method());
        if (ctx_ == nullptr) return false;
        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
        SSL_CTX_set_default_verify_paths(ctx_);
        ssl_ = SSL_new(ctx_);
        if (ssl_ == nullptr) return false;
        SSL_set_tlsext_host_name(ssl_, host_.c_str());
        SSL_set1_host(ssl_, host_.c_str());
        SSL_set_fd(ssl_, fd_);
        if (SSL_connect(ssl_) != 1) return false;
        X509* peer = SSL_get_peer_certificate(ssl_);
        if (peer == nullptr) return false;
        X509_free(peer);
        return true;
    }

    [[nodiscard]] bool handshake() noexcept {
        std::uint8_t raw_key[16];
        if (RAND_bytes(raw_key, sizeof(raw_key)) != 1) {
            for (std::size_t i = 0; i < sizeof(raw_key); ++i) raw_key[i] = static_cast<std::uint8_t>(i * 31 + 7);
        }
        static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        char b64[32];
        int b64len = 0;
        for (int i = 0; i < 16; i += 3) {
            const std::uint32_t v = (static_cast<std::uint32_t>(raw_key[i]) << 16) |
                                    (i + 1 < 16 ? static_cast<std::uint32_t>(raw_key[i + 1]) << 8 : 0u) |
                                    (i + 2 < 16 ? static_cast<std::uint32_t>(raw_key[i + 2]) : 0u);
            b64[b64len++] = kB64[(v >> 18) & 0x3F];
            b64[b64len++] = kB64[(v >> 12) & 0x3F];
            b64[b64len++] = (i + 1 < 16) ? kB64[(v >> 6) & 0x3F] : '=';
            b64[b64len++] = (i + 2 < 16) ? kB64[v & 0x3F] : '=';
        }
        b64[b64len] = '\0';
        char req[1024];
        const int n = std::snprintf(req, sizeof(req),
                                    "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\n"
                                    "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
                                    "Sec-WebSocket-Version: 13\r\n\r\n",
                                    path_.c_str(), host_.c_str(), b64);
        if (!ssl_write_all(reinterpret_cast<const std::uint8_t*>(req), static_cast<std::size_t>(n)))
            return false;
        char resp[2048];
        int total = 0;
        while (total < static_cast<int>(sizeof(resp)) - 1) {
            const int r = SSL_read(ssl_, resp + total, 1);
            if (r <= 0) {
                if (is_timeout(r)) continue;
                return false;
            }
            total += r;
            resp[total] = '\0';
            if (total >= 4 && std::strstr(resp, "\r\n\r\n") != nullptr) break;
        }
        return std::strstr(resp, "101") != nullptr;
    }

    std::string host_;
    std::string path_;
    int timeout_ms_;
    int fd_ = -1;
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    std::vector<std::uint8_t> frag_;
};

}  // namespace pmm::net
