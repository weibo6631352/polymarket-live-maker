// include/pmm/app/dotenv.hpp — 程序内置 .env 加载 (老板 2026-06-12「东西都在 .env 中了,
//   没必要多此一举, 程序直接读就好了」: 启动不再依赖 shell 先 source .env)。
//
// Owner: 老雷 (GM) | last_review: 2026-06-12
//
// 语义 (最小惊喜):
//   - 进程环境优先: 已存在的环境变量绝不覆盖 (setenv overwrite=0 同义) → 命令行
//     `LIVE_ARMED=0 ./trader_server ...` 仍可压过 .env。
//   - live 模式跳过 PAPER_MODE 键: .env 里的 PAPER_MODE=1 是 paper 跑法的文件默认值,
//     不该 veto 显式 --mode live (R-11 互斥闸只管「真实环境」的 PAPER_MODE, 在加载后检查,
//     systemd/shell 显式导出的冲突仍然 FATAL)。
//   - 格式: KEY=VALUE 每行; 支持 `export KEY=VALUE` 前缀 / 首尾空白 / 成对引号剥离;
//     跳过空行与 # 注释。解析失败的行静默跳过 (不因脏行拒启动)。
//   - §8 私钥红线: 只记录加载的【键名】, 值绝不进日志 — 调用方同样禁止打印值。
#pragma once

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace pmm::app {

// 解析并注入 .env。返回实际注入的键名列表 (仅名字, 供日志; 已在环境中/被跳过的不含)。
// path 不存在 → 返回空 (不是错误: 本地开发可无 .env, 凭证 fail-fast 在下游把关)。
inline std::vector<std::string> LoadDotEnv(const std::string& path, bool live_mode) {
    std::vector<std::string> loaded;
    std::ifstream f(path);
    if (!f.is_open()) return loaded;

    auto trim = [](std::string& s) {
        const char* ws = " \t\r\n";
        const auto b = s.find_first_not_of(ws);
        if (b == std::string::npos) { s.clear(); return; }
        const auto e = s.find_last_not_of(ws);
        s = s.substr(b, e - b + 1);
    };

    std::string line;
    while (std::getline(f, line)) {
        trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("export ", 0) == 0) line = line.substr(7);
        const auto eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;  // 脏行跳过
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        trim(key);
        trim(val);
        if (key.empty()) continue;
        // 成对引号剥离 ("..." 或 '...')
        if (val.size() >= 2 && (val.front() == '"' || val.front() == '\'') && val.back() == val.front())
            val = val.substr(1, val.size() - 2);
        // live 模式跳过 PAPER_MODE (文件默认值不 veto 显式 --mode live; 见头注)
        if (live_mode && key == "PAPER_MODE") continue;
        // 进程环境优先 (overwrite=0)
        if (::getenv(key.c_str()) != nullptr) continue;
        if (::setenv(key.c_str(), val.c_str(), /*overwrite=*/0) == 0) loaded.push_back(key);
    }
    return loaded;
}

}  // namespace pmm::app
