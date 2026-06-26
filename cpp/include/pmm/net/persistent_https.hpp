// pmm/net/persistent_https.hpp — 持久化 (keep-alive) HTTPS GET 客户端 (热链高频轮询)
//
// Owner: 老雷 (GM) — 2026-06-04 老板「主动实时查询订单簿压着官方限速, 全市场共享 149hz, 用热链」。
//   PM /book 单 token 限速 150 req/s; SeedTokensFromRest 的 popen curl 每请求冷连 (TLS 握手 ~30ms)
//   太慢, 跑不满 149hz。本客户端持久一条 TLS 连接复用, GET 之间不重握手 → 可压满限速。
//
// 设计:
//   - 单线程使用 (调用方在 ActiveBookPoller 线程内独占; 非线程安全, 不共享)。
//   - SSL_write/SSL_read 失败 → shut() 关连接, 下次 Get() 自动重连 (热链自愈)。
//   - 带 socket 读写超时 (防 PM 卡住整个轮询线程)。
//   - 仅 GET (轮询只读 /book); 不含凭证 (公开端点, URL 无 key — 红线: 不记 URL 到日志)。
//
// 红线: 不在 WSS event loop 调用 (本客户端在独立 ActiveBookPoller 线程, R-12 合规)。
#pragma once

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>

#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace pmm::net {

class PersistentHttps {
public:
    explicit PersistentHttps(std::string host, int timeout_ms = 4000) noexcept
        : host_(std::move(host)), timeout_ms_(timeout_ms) {}
    ~PersistentHttps() { Shut(); }
    PersistentHttps(const PersistentHttps&) = delete;
    PersistentHttps& operator=(const PersistentHttps&) = delete;

    // Get — 复用持久连接发 GET path; 返回 response body (chunked/content-length 解完整)。
    //   失败 (连接断/超时/非 2xx 不区分) → 返回空串 + 关连接 (下次 Get 重连)。
    [[nodiscard]] std::string Get(const std::string& path) noexcept {
        last_status_ = 0;
        if (ssl_ == nullptr && !Open()) return {};
        std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host_ +
                          "\r\nConnection: keep-alive\r\nAccept: application/json\r\n\r\n";
        if (SSL_write(ssl_, req.data(), static_cast<int>(req.size())) <= 0) {
            Shut();
            return {};
        }
        std::string buf;
        char c[8192];
        int n;
        std::size_t hp;
        while ((hp = buf.find("\r\n\r\n")) == std::string::npos) {
            n = SSL_read(ssl_, c, sizeof(c));
            if (n <= 0) {
                Shut();
                return {};
            }
            buf.append(c, static_cast<std::size_t>(n));
            if (buf.size() > kMaxResp) {
                Shut();
                return {};
            }
        }
        std::string head = buf.substr(0, hp);
        std::string body = buf.substr(hp + 4);
        // 状态行 "HTTP/1.1 200 OK" → 200 (供调用方区分 2xx/4xx/5xx)。失败保持 0。
        // 只在首行 (首个 CRLF 前) 找空格, 避免畸形头让空格落到后面的 header 行上。
        {
            const std::size_t eol = head.find("\r\n");
            const std::string status_line = (eol == std::string::npos) ? head : head.substr(0, eol);
            const std::size_t sp = status_line.find(' ');
            if (sp != std::string::npos) last_status_ = std::atoi(status_line.c_str() + sp + 1);
        }
        std::string lh = head;
        std::transform(lh.begin(), lh.end(), lh.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        // chunked
        if (lh.find("transfer-encoding: chunked") != std::string::npos) {
            std::string de;
            while (true) {
                std::size_t e;
                while ((e = body.find("\r\n")) == std::string::npos) {
                    n = SSL_read(ssl_, c, sizeof(c));
                    if (n <= 0) {
                        Shut();
                        return de;
                    }
                    body.append(c, static_cast<std::size_t>(n));
                }
                long sz = std::strtol(body.substr(0, e).c_str(), nullptr, 16);
                body.erase(0, e + 2);
                if (sz <= 0) break;
                while (static_cast<long>(body.size()) < sz + 2) {
                    n = SSL_read(ssl_, c, sizeof(c));
                    if (n <= 0) {
                        Shut();
                        return de;
                    }
                    body.append(c, static_cast<std::size_t>(n));
                }
                de.append(body, 0, static_cast<std::size_t>(sz));
                body.erase(0, static_cast<std::size_t>(sz) + 2);
                if (de.size() > kMaxResp) {
                    Shut();
                    return de;
                }
            }
            return de;
        }
        // content-length (在行边界匹配, 头字段总在 "\r\n" 之后 — 避免误匹配 x-...-content-length: 之类子串)
        auto cl = lh.find("\r\ncontent-length:");
        long len = (cl != std::string::npos) ? std::atol(lh.c_str() + cl + 17) : -1;
        if (len >= 0) {
            while (static_cast<long>(body.size()) < len) {
                n = SSL_read(ssl_, c, sizeof(c));
                if (n <= 0) {
                    Shut();
                    break;
                }
                body.append(c, static_cast<std::size_t>(n));
            }
            return body.substr(0, std::min(static_cast<long>(body.size()), len));
        }
        return body;
    }

    [[nodiscard]] bool connected() const noexcept { return ssl_ != nullptr; }
    // 上次 Get 的 HTTP 状态码 (0 = 连接/超时失败)。
    [[nodiscard]] int last_status() const noexcept { return last_status_; }

private:
    static constexpr std::size_t kMaxResp = 4 * 1024 * 1024;  // 4MB 上限 (防异常巨包)

    bool Open() noexcept {
        Shut();
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
        // socket 读写超时 (防卡死轮询线程)
        struct timeval tv{};
        tv.tv_sec = timeout_ms_ / 1000;
        tv.tv_usec = (timeout_ms_ % 1000) * 1000;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        // 非阻塞 connect + poll 超时 (2026-06-14 网络审计): Linux 上 SO_*TIMEO 不影响 connect() 本身,
        //   跨洋目标 DROP SYN 时阻塞 connect 默认 ~127s(内核 SYN 重传)→ 冻结整个 ActiveBookPoller 线程,
        //   book 价格停更、决策跑陈旧数据。改非阻塞 connect + poll(timeout_ms_) 上限, 然后恢复阻塞。
        const int fl = ::fcntl(fd_, F_GETFL, 0);
        ::fcntl(fd_, F_SETFL, fl | O_NONBLOCK);
        int crc = ::connect(fd_, res->ai_addr, res->ai_addrlen);
        if (crc != 0 && errno != EINPROGRESS) {
            freeaddrinfo(res);
            Shut();
            return false;
        }
        if (crc != 0) {  // EINPROGRESS: 等可写(连上)或超时
            struct pollfd pfd{};
            pfd.fd = fd_;
            pfd.events = POLLOUT;
            const int pr = ::poll(&pfd, 1, timeout_ms_);
            int soerr = 0;
            socklen_t sl = sizeof(soerr);
            if (pr <= 0 || ::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &soerr, &sl) != 0 || soerr != 0) {
                freeaddrinfo(res);
                Shut();
                return false;
            }
        }
        ::fcntl(fd_, F_SETFL, fl);  // 恢复阻塞 (后续 SSL_connect/读写用阻塞 + SO_*TIMEO)
        freeaddrinfo(res);
        ctx_ = SSL_CTX_new(TLS_client_method());
        if (ctx_ == nullptr) {
            Shut();
            return false;
        }
        // TLS 加固 (2026-06-14 网络审计; 与 live_wss_transport.hpp:332-381 一致): 验证服务端证书链 +
        //   hostname。原仅设 SNI 无校验 → 149Hz 喂价热链对 MITM 透明, 攻击者可注入伪造 bid/ask 诱发反向下单。
        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
        SSL_CTX_set_default_verify_paths(ctx_);
        ssl_ = SSL_new(ctx_);
        if (ssl_ == nullptr) {
            Shut();
            return false;
        }
        SSL_set_tlsext_host_name(ssl_, host_.c_str());
        SSL_set1_host(ssl_, host_.c_str());  // CN/SAN hostname 校验 (RFC 6125), SSL_connect 时自动比对
        SSL_set_fd(ssl_, fd_);
        if (SSL_connect(ssl_) != 1) {
            Shut();
            return false;
        }
        // VERIFY_PEER 下 SSL_connect!=1 已挡掉验证失败; 显式确认 peer cert 已呈现 (兜底)。
        X509* peer = SSL_get_peer_certificate(ssl_);
        if (peer == nullptr) {
            Shut();
            return false;
        }
        X509_free(peer);
        return true;
    }

    void Shut() noexcept {
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
    }

    std::string host_;
    int timeout_ms_;
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    int fd_ = -1;
    int last_status_ = 0;
};

}  // namespace pmm::net
