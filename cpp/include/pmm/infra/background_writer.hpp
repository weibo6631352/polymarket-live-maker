// include/pmm/infra/background_writer.hpp — 异步缓冲落盘器 (2026-06-13, 老板「独立线程异步写 + 缓冲写不高频读写磁盘」)
//
// 决策线程 enqueue (纳秒, 不阻塞), 独立 writer 线程【批量缓冲】写盘 → 把磁盘 IO 与尾延迟和决策环隔离。
//   - AppendLine: 追加一行到文件; writer 攒 per-file 缓冲, 满阈值 OR 定时才 flush → 不高频读写磁盘 (秒级批量)。
//   - 有界队列, 满即丢弃 + 计数 (绝不阻塞生产者; journal 容忍丢失, 内存账本才是真值源)。
//   - 持久 FILE* (开一次复用, 省 fopen/fclose churn)。析构: 停 + drain + flush + close + join。
//   - 用途: 纯写出 sink (fills journal) — 不参与热路径一致性 (不像下单/账本, 故异步安全)。
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace pmm::infra {

class BackgroundWriter {
public:
    BackgroundWriter() : th_([this] { Run_(); }) {}
    ~BackgroundWriter() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_one();
        if (th_.joinable()) th_.join();
    }
    BackgroundWriter(const BackgroundWriter&) = delete;
    BackgroundWriter& operator=(const BackgroundWriter&) = delete;

    // 决策线程调用: 入队一行 (移动 path+line)。队列满 → 丢弃 + dropped_++。纳秒级, 绝不阻塞。
    void AppendLine(std::string path, std::string line) noexcept {
        bool notify = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (q_.size() >= kMaxQueue) {  // 满 → 丢 (不阻塞决策环, 这正是异步的全部意义)
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            q_.push_back(Item{std::move(path), std::move(line)});
            ++enqueued_;
            notify = true;
        }
        if (notify) cv_.notify_one();
    }

    // 决策线程调用: 入队"原子替换整个文件"(writer 线程写 .tmp + rename), 非追加。用于账本快照等需整体替换语义
    //   的小文件 → 决策环零磁盘阻塞 (2026-06-14 网络/性能审计 P0: 原 SaveLedgerSnapshot 在 loop_thread_ 同步
    //   fopen/fprintf×N/rename, 每60s 卡决策环 5-100ms)。纳秒级入队, 实际 IO 全在 writer 线程。
    void WriteFileAtomic(std::string path, std::string content) noexcept {
        bool notify = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (q_.size() >= kMaxQueue) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            q_.push_back(Item{std::move(path), std::move(content), true});
            ++enqueued_;
            notify = true;
        }
        if (notify) cv_.notify_one();
    }

    // 同步等待: 阻塞到【调用时刻已入队的全部项】都被 writer 线程处理完(含 replace 的 tmp+rename 落盘)。
    //   用于测试 Save→Restore round-trip + 生产关停前确保最终快照落盘。调用后入队的不阻塞。
    void Flush() noexcept {
        std::unique_lock<std::mutex> lk(mu_);
        const std::uint64_t target = enqueued_;
        flush_cv_.wait(lk, [this, target] { return processed_ >= target || stop_; });
    }

    // 强制把已缓冲的 append 数据写进文件 (到 OS page cache), 阻塞到 writer 完成一轮全 sink flush。
    // 先 Flush() 再 FlushToDisk() 才能保证"已入队的事件"真正落到文件 (关停/取快照前用; 对齐 Python close())。
    void FlushToDisk() noexcept {
        std::unique_lock<std::mutex> lk(mu_);
        const std::uint64_t target = ++disk_req_;
        cv_.notify_one();  // 唤醒 writer 处理本次落盘请求
        disk_cv_.wait(lk, [this, target] { return disk_done_ >= target || stop_; });
    }

    [[nodiscard]] std::uint64_t dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
    struct Item {
        std::string path;
        std::string line;
        bool replace{false};  // true=原子替换整文件(写tmp+rename); false=追加缓冲
    };
    struct Sink {
        std::FILE* fp{nullptr};
        std::string buf;  // 累积缓冲 (满阈值/定时才落盘)
    };

    static constexpr std::size_t kMaxQueue = 8192;          // 有界 (满即丢; 8192×~640B ≈ 5MB 上限)
    static constexpr std::size_t kFlushBytes = 64 * 1024;   // 缓冲阈值 → 批量写一次
    static constexpr int kFlushIntervalMs = 1000;           // 定时 flush 上限 (秒级落盘, 不高频读写磁盘)

    void Run_() {
        using clock = std::chrono::steady_clock;
        auto last_flush = clock::now();
        for (;;) {
            std::deque<Item> batch;
            bool stopping = false;
            std::uint64_t disk_target = 0;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait_for(lk, std::chrono::milliseconds(kFlushIntervalMs),
                             [this] { return stop_ || !q_.empty() || disk_req_ > disk_done_; });
                batch.swap(q_);  // 一次性取走整批 (writer 持锁极短)
                stopping = stop_;
                disk_target = disk_req_;
            }
            for (auto& it : batch) {
                if (it.replace) {
                    WriteFileAtomic_(it.path, it.line);  // 整文件原子替换 (写tmp+rename), 不进追加缓冲
                } else {
                    sinks_[it.path].buf += it.line;  // 入 per-file 缓冲 (内存)
                }
            }
            const bool time_up = clock::now() - last_flush >= std::chrono::milliseconds(kFlushIntervalMs);
            for (auto& [path, sink] : sinks_) {
                if (stopping || time_up || sink.buf.size() >= kFlushBytes) FlushSink_(path, sink);
            }
            if (time_up || stopping) last_flush = clock::now();
            if (disk_target > disk_done_) {  // FlushToDisk() 请求: 强制把所有 sink 缓冲落盘
                for (auto& [path, sink] : sinks_) FlushSink_(path, sink);
                std::lock_guard<std::mutex> lk(mu_);
                disk_done_ = disk_target;
                disk_cv_.notify_all();
            }
            if (!batch.empty()) {  // Flush() 同步点: 标记本批已处理 (replace 已 rename 落盘; append 已入缓冲)
                std::lock_guard<std::mutex> lk(mu_);
                processed_ += static_cast<std::uint64_t>(batch.size());
                flush_cv_.notify_all();
            }
            if (stopping) break;  // stop: 本轮已 drain+flush batch, 退出
        }
        for (auto& [path, sink] : sinks_) {  // 终关: 兜底 flush + 关 handle
            FlushSink_(path, sink);
            if (sink.fp != nullptr) std::fclose(sink.fp);
        }
    }

    // 原子替换整文件 (writer 线程内): 写 .tmp + rename。tmp 复用 path+".tmp"。失败静默 (账本快照容忍单次丢失,
    //   60s 后下次再写; 内存账本才是真值源)。
    static void WriteFileAtomic_(const std::string& path, const std::string& content) {
        const std::string tmp = path + ".tmp";
        std::FILE* fp = std::fopen(tmp.c_str(), "w");
        if (fp == nullptr) return;
        std::fwrite(content.data(), 1, content.size(), fp);
        std::fclose(fp);
        std::rename(tmp.c_str(), path.c_str());
    }

    void FlushSink_(const std::string& path, Sink& sink) {
        if (sink.buf.empty()) return;
        if (sink.fp == nullptr) sink.fp = std::fopen(path.c_str(), "a");  // 持久 handle, 开一次
        if (sink.fp == nullptr) { sink.buf.clear(); return; }  // 打不开 (目录缺等) → 丢缓冲, 不卡死
        std::fwrite(sink.buf.data(), 1, sink.buf.size(), sink.fp);
        std::fflush(sink.fp);  // 批量一次 write() 到 OS page cache (非 fsync; OS writeback 落盘)
        sink.buf.clear();
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::condition_variable flush_cv_;       // Flush() 等此 cv; writer 处理完一批后 notify
    std::condition_variable disk_cv_;        // FlushToDisk() 等此 cv; writer 落盘一轮后 notify
    std::deque<Item> q_;
    bool stop_{false};
    std::uint64_t enqueued_{0};              // 累计入队项数 (mu_ 保护); Flush() 的目标
    std::uint64_t processed_{0};             // 累计已处理项数 (mu_ 保护); writer 每批后 +=
    std::uint64_t disk_req_{0};              // FlushToDisk 请求序号 (mu_)
    std::uint64_t disk_done_{0};             // 已完成的落盘请求序号 (mu_ 写, writer 读)
    std::atomic<std::uint64_t> dropped_{0};
    std::unordered_map<std::string, Sink> sinks_;  // 仅 writer 线程访问 (无需锁)
    std::thread th_;  // 末声明: 其他成员先就绪, 再启 writer 线程
};

}  // namespace pmm::infra
