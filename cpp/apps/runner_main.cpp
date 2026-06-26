// apps/runner_main.cpp — live-maker 入口: 自主做市轮询循环 (port of pm_trader/runner.py main)。
//
// 运行: ./live-maker   (从 .env / 环境读取配置; 默认 dry-run/dry-live, PM_TRADER_LIVE=1 才下真单)
// 防多开: 同一 state_dir (同一钱包/账本) 只允许一个实例 (flock on <state_dir>/live-maker.pid)。
#include <cstdio>
#include <exception>

#include "pmm/config.hpp"
#include "pmm/infra/process/single_instance.hpp"
#include "pmm/runner.hpp"

int main() {
    const pmm::RunnerConfig cfg = pmm::RunnerConfig::from_env();
    try {
        // 先抢锁: 两个进程跑同一账本 = 重复下单 + 账本打架。失败立即退出, 绝不并跑。
        const pmm::infra::process::SingleInstanceLock lock(cfg.state_dir + "/live-maker.pid");
        pmm::LiveRunner runner(cfg);
        runner.run();  // SIGTERM → 优雅撤单/平仓后返回 → lock RAII 释放
    } catch (const pmm::infra::process::SingleInstanceLockFailure& e) {
        std::fprintf(stderr, "refusing to start: %s\n", e.what());
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
    return 0;
}
