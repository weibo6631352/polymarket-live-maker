cmake_minimum_required(VERSION 3.27)
project(pmm
    VERSION 0.1.0
    DESCRIPTION "Polymarket live liquidity-rewards market maker (C++ port of pm_trader)"
    LANGUAGES CXX C
)
# C 语言: secp256k1 (Bitcoin Core, FetchContent) 是 C 工程, 必须 enable C。

# C++20 锁 (沿用 sports-trader-cpp ADR-001)。
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# ---- 编译警告 (吸收 sports-trader-cpp 的严格构建卫生) ----
# C++-only 警告用 generator expression 限制, 避免 secp256k1 的 C 源报错。
add_compile_options(
    -Wall -Wextra
    $<$<COMPILE_LANGUAGE:CXX>:-Wshadow>
    $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>
    $<$<COMPILE_LANGUAGE:CXX>:-Wcast-align>
    $<$<COMPILE_LANGUAGE:CXX>:-Wold-style-cast>
    $<$<COMPILE_LANGUAGE:CXX>:-Wconversion>
    $<$<COMPILE_LANGUAGE:CXX>:-Wsign-conversion>
    $<$<COMPILE_LANGUAGE:CXX>:-Wnull-dereference>
    $<$<COMPILE_LANGUAGE:CXX>:-Wdouble-promotion>
    $<$<COMPILE_LANGUAGE:CXX>:-Wformat=2>
    -fno-omit-frame-pointer
)
# -Werror 暂以 option 控制 (默认 OFF, 迁移成形后转 ON)。
option(PMM_WERROR "Treat warnings as errors" OFF)
if(PMM_WERROR)
    add_compile_options(-Werror)
endif()

# Sanitizers (开发期, 不进 release)。
option(PMM_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(PMM_ENABLE_UBSAN "Enable UBSan" OFF)
option(PMM_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
if(PMM_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-sanitize-recover=address)
    add_link_options(-fsanitize=address)
endif()
if(PMM_ENABLE_UBSAN)
    add_compile_options(-fsanitize=undefined -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=undefined)
endif()
if(PMM_ENABLE_TSAN AND NOT PMM_ENABLE_ASAN)
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
endif()

include_directories(${CMAKE_SOURCE_DIR}/include)

# ---- OpenSSL (HMAC-SHA256 for CLOB L2 auth; TLS for PersistentHttps) ----
find_package(OpenSSL QUIET)
if(NOT OpenSSL_FOUND)
    # Homebrew 手动回退 (同 sports-trader-cpp app/CMakeLists)。
    find_path(OPENSSL_INCLUDE_DIR openssl/ssl.h
        PATHS /opt/homebrew/opt/openssl@3/include /opt/homebrew/opt/openssl/include NO_DEFAULT_PATH)
    find_library(OPENSSL_SSL_LIBRARY ssl
        PATHS /opt/homebrew/opt/openssl@3/lib /opt/homebrew/opt/openssl/lib NO_DEFAULT_PATH)
    find_library(OPENSSL_CRYPTO_LIBRARY crypto
        PATHS /opt/homebrew/opt/openssl@3/lib /opt/homebrew/opt/openssl/lib NO_DEFAULT_PATH)
    if(NOT OPENSSL_INCLUDE_DIR OR NOT OPENSSL_CRYPTO_LIBRARY)
        message(FATAL_ERROR "OpenSSL not found. brew install openssl@3")
    endif()
    add_library(OpenSSL::Crypto UNKNOWN IMPORTED)
    set_target_properties(OpenSSL::Crypto PROPERTIES
        IMPORTED_LOCATION ${OPENSSL_CRYPTO_LIBRARY}
        INTERFACE_INCLUDE_DIRECTORIES ${OPENSSL_INCLUDE_DIR})
    add_library(OpenSSL::SSL UNKNOWN IMPORTED)
    set_target_properties(OpenSSL::SSL PROPERTIES
        IMPORTED_LOCATION ${OPENSSL_SSL_LIBRARY}
        INTERFACE_INCLUDE_DIRECTORIES ${OPENSSL_INCLUDE_DIR}
        INTERFACE_LINK_LIBRARIES OpenSSL::Crypto)
endif()

# ---- secp256k1 v0.6.0 (Ethereum ECDSA, recovery module) ----
# 离线复用: configure 时传 -DFETCHCONTENT_SOURCE_DIR_SECP256K1=<已解压源码目录> 跳过下载。
include(FetchContent)
set(SECP256K1_ENABLE_MODULE_RECOVERY ON  CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_BENCHMARK        OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_TESTS            OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_EXHAUSTIVE_TESTS OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_CTIME_TESTS      OFF CACHE BOOL "" FORCE)
set(SECP256K1_INSTALL                OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS                OFF CACHE BOOL "" FORCE)
FetchContent_Declare(secp256k1
    URL      "https://github.com/bitcoin-core/secp256k1/archive/refs/tags/v0.6.0.tar.gz"
    URL_HASH SHA256=785bb98e7d6705c51c8dfa8ac3af6aa2ccfa3774714d51c0b9e28fac1146e9f1
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(secp256k1)

# ---- pmm_crypto: 复用 EIP-712 + secp256k1 + CLOB wire 签名链 ----
add_library(pmm_crypto STATIC
    src/pmm/crypto/eip712_v2.cpp
    src/pmm/crypto/secp256k1_signer.cpp
    src/pmm/wire/clob_wire.cpp
    # CTF merge 平仓: 纯 ABI calldata 编码 + RLP + EIP-1559 tx 签名 (复用 keccak/secp256k1, 无 curl)。
    src/pmm/chain/rlp.cpp
    src/pmm/chain/ctf.cpp
    src/pmm/chain/proxy_exec.cpp  # sig_type=1 代理 exec 包装 (纯 keccak+abi 编码)
    src/pmm/chain/eip1559.cpp
)
target_compile_features(pmm_crypto PUBLIC cxx_std_20)
target_link_libraries(pmm_crypto
    PRIVATE secp256k1 OpenSSL::Crypto
)

# ---- SQLite3 (持久化) ----
find_package(SQLite3 REQUIRED)

# ---- libcurl (ClobSubmitter 的实盘下单 POST/DELETE + L1/L2 鉴权; 读路径仍用 PersistentHttps) ----
find_package(CURL REQUIRED)

# ---- nlohmann/json (vendored single header, 解析 Gamma/CLOB 响应 + 构造事件 JSON) ----
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE ${CMAKE_SOURCE_DIR}/third_party)

# ---- pmm_core: 业务逻辑库 (随迁移逐模块增长) ----
# 全量遥测 (Fast-DDS): WITH_TELEMETRY=ON (盒子, 见 ~/dds) 链 Fast-DDS + 编 dds_publisher.cpp;
# 默认 OFF → 编 no-op publisher.cpp (Mac 本地/单测零依赖, publish 全空操作)。
option(WITH_TELEMETRY "Build Fast-DDS telemetry publisher" OFF)
if(WITH_TELEMETRY)
    # ---- Fast-DDS codegen (盒子构建: cmake -DCMAKE_PREFIX_PATH=$HOME/dds) ----
    # fastddsgen 把 telemetry.idl 生成 PubSubTypes/TypeObjectSupport .cxx; dds_publisher.cpp include 之。
    find_package(fastcdr REQUIRED)
    find_package(fastdds REQUIRED)

    set(FASTDDSGEN "$ENV{HOME}/dds-build/Fast-DDS-Gen/scripts/fastddsgen"
        CACHE FILEPATH "Path to fastddsgen launcher")

    set(TGEN ${CMAKE_BINARY_DIR}/telemetry_gen)
    file(MAKE_DIRECTORY ${TGEN})

    # WORKING_DIRECTORY = idl 所在目录 → fastddsgen 把绝对 idl 路径相对化为 basename, 输出平铺到 ${TGEN}
    # (否则会把整条目录路径复刻到 ${TGEN} 下, OUTPUT 路径无法预测)。-replace 覆盖旧产物。
    add_custom_command(
        OUTPUT
            ${TGEN}/telemetryPubSubTypes.cxx
            ${TGEN}/telemetryTypeObjectSupport.cxx
        COMMAND ${FASTDDSGEN} -replace -d ${TGEN}
                ${CMAKE_CURRENT_SOURCE_DIR}/telemetry/telemetry.idl
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/telemetry
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/telemetry/telemetry.idl
        COMMENT "fastddsgen: telemetry.idl -> ${TGEN}"
        VERBATIM
    )

    set(PMM_TELEMETRY_SRC
        src/pmm/telemetry/dds_publisher.cpp
        ${TGEN}/telemetryPubSubTypes.cxx
        ${TGEN}/telemetryTypeObjectSupport.cxx
    )
else()
    set(PMM_TELEMETRY_SRC src/pmm/telemetry/publisher.cpp)
endif()

add_library(pmm_core STATIC
    src/pmm/config.cpp
    src/pmm/db.cpp
    src/pmm/orderbook.cpp
    src/pmm/events.cpp
    src/pmm/analytics.cpp
    src/pmm/api.cpp
    src/pmm/tail_vendor.cpp
    src/pmm/rewards.cpp
    src/pmm/portfolio.cpp
    src/pmm/maker_live.cpp
    src/pmm/clob_submitter.cpp
    src/pmm/engine.cpp
    src/pmm/ws.cpp
    src/pmm/runner.cpp
    src/pmm/maker_sim.cpp
    src/pmm/review.cpp
    # Polygon JSON-RPC 客户端 (nonce/gas/sendRawTx) + merge 平仓决策/双闸执行器 (curl + json)。
    src/pmm/chain/polygon_rpc.cpp
    src/pmm/chain/merge_flatten.cpp
    ${PMM_TELEMETRY_SRC}
)
target_compile_features(pmm_core PUBLIC cxx_std_20)
# OpenSSL::SSL: PersistentHttps 的 TLS 客户端 (带 openssl 头路径 + libssl/libcrypto)。
target_link_libraries(pmm_core PUBLIC pmm_crypto SQLite::SQLite3 nlohmann_json OpenSSL::SSL CURL::libcurl)

# ---- git commit 溯源 (ConfigSnapshot.git_commit): configure 时取短哈希烤进 pmm_core ----
# 反映 configure 时的 HEAD (干净的 git-reset 部署足够); 取不到则不定义 → runner 发 ""。
execute_process(
    COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR} rev-parse --short HEAD
    OUTPUT_VARIABLE PMM_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(PMM_GIT_COMMIT)
    target_compile_definitions(pmm_core PRIVATE PMM_GIT_COMMIT="${PMM_GIT_COMMIT}")
endif()

# ---- Fast-DDS 链接 (仅 WITH_TELEMETRY=ON): 生成头 include 路径 + 编译开关 + fastdds/fastcdr ----
if(WITH_TELEMETRY)
    # 生成的 PubSubTypes/CdrAux 含 eProsima 内部转换, -Wconversion/-Wold-style-cast 会大量误报 → 关警告。
    set_source_files_properties(
        ${TGEN}/telemetryPubSubTypes.cxx
        ${TGEN}/telemetryTypeObjectSupport.cxx
        PROPERTIES COMPILE_OPTIONS "-w"
    )
    target_include_directories(pmm_core PRIVATE ${TGEN})
    target_compile_definitions(pmm_core PRIVATE PMM_WITH_TELEMETRY)
    target_link_libraries(pmm_core PRIVATE fastdds fastcdr)
endif()

# ---- 测试 (tests/): smoke + 数值 parity 自检 (apps/ 只放可发布程序) ----
add_executable(pmm_smoke tests/smoke_main.cpp)
target_link_libraries(pmm_smoke PRIVATE pmm_crypto)

add_executable(pmm_db_smoke tests/db_smoke_main.cpp)
target_link_libraries(pmm_db_smoke PRIVATE pmm_core)

add_executable(pmm_config_smoke tests/config_smoke_main.cpp)
target_link_libraries(pmm_config_smoke PRIVATE pmm_core)

add_executable(pmm_orderbook_parity tests/orderbook_parity_main.cpp)
target_link_libraries(pmm_orderbook_parity PRIVATE pmm_core)

add_executable(pmm_ratelimit_smoke tests/ratelimit_smoke_main.cpp)
target_link_libraries(pmm_ratelimit_smoke PRIVATE pmm_core)

add_executable(pmm_events_smoke tests/events_smoke_main.cpp)
target_link_libraries(pmm_events_smoke PRIVATE pmm_core)

add_executable(pmm_analytics_parity tests/analytics_parity_main.cpp)
target_link_libraries(pmm_analytics_parity PRIVATE pmm_core)

add_executable(pmm_api_rewards_parity tests/api_rewards_parity_main.cpp)
target_link_libraries(pmm_api_rewards_parity PRIVATE pmm_core)

add_executable(pmm_portfolio_parity tests/portfolio_parity_main.cpp)
target_link_libraries(pmm_portfolio_parity PRIVATE pmm_core)

add_executable(pmm_maker_parity tests/maker_parity_main.cpp)
target_link_libraries(pmm_maker_parity PRIVATE pmm_core)

add_executable(pmm_review_fixes tests/review_fixes_main.cpp)
target_link_libraries(pmm_review_fixes PRIVATE pmm_core)

add_executable(pmm_clob_amounts_parity tests/clob_amounts_parity_main.cpp)
target_link_libraries(pmm_clob_amounts_parity PRIVATE pmm_core)

add_executable(pmm_orders_smoke tests/orders_smoke_main.cpp)
target_link_libraries(pmm_orders_smoke PRIVATE pmm_core)

add_executable(pmm_poll_fills_cursor tests/poll_fills_cursor_main.cpp)
target_link_libraries(pmm_poll_fills_cursor PRIVATE pmm_core)

add_executable(pmm_engine_smoke tests/engine_smoke_main.cpp)
target_link_libraries(pmm_engine_smoke PRIVATE pmm_core)

add_executable(pmm_ws_smoke tests/ws_smoke_main.cpp)
target_link_libraries(pmm_ws_smoke PRIVATE pmm_core)

add_executable(pmm_runner_smoke tests/runner_smoke_main.cpp)
target_link_libraries(pmm_runner_smoke PRIVATE pmm_core)

add_executable(pmm_lock_smoke tests/lock_smoke_main.cpp)
target_link_libraries(pmm_lock_smoke PRIVATE pmm_core)

add_executable(pmm_maker_sim_parity tests/maker_sim_parity_main.cpp)
target_link_libraries(pmm_maker_sim_parity PRIVATE pmm_core)

add_executable(pmm_review_parity tests/review_parity_main.cpp)
target_link_libraries(pmm_review_parity PRIVATE pmm_core)

# CTF merge 平仓: DRY 编码/签名验证 (calldata + EIP-1559 raw tx 逐字节对齐 eth_abi/eth_account 参考)。
add_executable(pmm_merge_smoke tests/merge_encode_main.cpp)
target_include_directories(pmm_merge_smoke PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(pmm_merge_smoke PRIVATE pmm_core)

# ---- 生产入口: live-maker (自主做市轮询循环) ----
add_executable(live-maker apps/runner_main.cpp)
target_link_libraries(live-maker PRIVATE pmm_core)

# ---- 分析工具: review-report (估计 vs 链上实际奖励对账) ----
add_executable(review-report apps/review_main.cpp)
target_link_libraries(review-report PRIVATE pmm_core)

# ---- live 烟测: order-smoke (受控验证 C++ V2 签名被 CLOB 接受; 远离 mid 挂单+撤单) ----
add_executable(order-smoke apps/order_smoke_main.cpp)
target_link_libraries(order-smoke PRIVATE pmm_core)

# ---- tail-seller: up-tail premium seller (GTC BUY NO, hold to resolution; dry by default,
# double-gated PM_TRADER_LIVE+TS_ARM; hard caps on order count + notional) ----
add_executable(tail-seller apps/tail_seller_main.cpp)
target_link_libraries(tail-seller PRIVATE pmm_core)

# ---- tail-vendor: 常驻系统性上行尾卖方 (tail-seller 泛化; 纯函数决策核 + gamma 扫描循环) ----
add_executable(tail-vendor apps/tail_vendor_main.cpp)
target_link_libraries(tail-vendor PRIVATE pmm_core)
add_executable(pmm_tail_vendor_test tests/tail_vendor_test_main.cpp)
target_link_libraries(pmm_tail_vendor_test PRIVATE pmm_core)

# ---- 只读校验: balance-check (打印签名地址 + 查 CLOB 抵押余额; 零下单) ----
add_executable(balance-check apps/balance_check_main.cpp)
target_link_libraries(balance-check PRIVATE pmm_core)

# ---- scalp-trial: 硬限额真钱 crypto-scalp 试验 (Kraken WS 价 + gamma 窗口 + ClobSubmitter; $30 死止损 + 双重门) ----
add_executable(scalp-trial apps/scalp_trial_main.cpp)
target_link_libraries(scalp-trial PRIVATE pmm_core)

# ---- wss-probe: Phase 0 sniper plan — measure PM CLOB WSS rate/granularity/format (read-only) ----
add_executable(wss-probe apps/wss_probe_main.cpp)
target_link_libraries(wss-probe PRIVATE pmm_core)

# ---- binance-sniper: DRY-LIVE crypto-micro sniper (verified seconds-Binance-move edge); orders gated ----
add_executable(binance-sniper apps/binance_sniper_main.cpp)
target_link_libraries(binance-sniper PRIVATE pmm_core)

# ---- okx-sniper: EXTREME-low-latency sniper (OKX signal + PM WSS book + busy-poll); orders double-gated ----
add_executable(okx-sniper apps/okx_sniper_main.cpp)
target_link_libraries(okx-sniper PRIVATE pmm_core)

# ---- 人工平仓: flatten (市价平掉未跟踪库存换回 USDC; REAL ORDER) ----
add_executable(flatten apps/flatten_main.cpp)
target_link_libraries(flatten PRIVATE pmm_core)

# ---- DRY 工具: merge-encode (打印 CTF mergePositions calldata + 已签 raw tx; 绝不广播) ----
add_executable(merge-encode apps/merge_encode_main.cpp)
target_link_libraries(merge-encode PRIVATE pmm_core)

# ---- 遥测消费侧: telemetry_tap (Fast-DDS 订阅者 → stdout JSON; 仅 WITH_TELEMETRY) ----
# 桥接 web 仪表盘 (ws_server.py) + 记录器 (recorder.py)。pmm_core PRIVATE 链 fastdds 不传递, 故显式再链。
if(WITH_TELEMETRY)
    add_executable(telemetry_tap telemetry/tap/tap.cpp)
    target_include_directories(telemetry_tap PRIVATE ${TGEN})
    target_link_libraries(telemetry_tap PRIVATE pmm_core fastdds fastcdr)

    # 外部 curator 闭环工具 (host 侧): curate_read 接收候选 (PoolEval) + curate_push 推白名单 (CuratorCommand)。
    add_executable(curate_read telemetry/curate_read/curate_read.cpp)
    target_include_directories(curate_read PRIVATE ${TGEN})
    target_link_libraries(curate_read PRIVATE pmm_core fastdds fastcdr)

    add_executable(curate_push telemetry/curate_push/curate_push.cpp)
    target_include_directories(curate_push PRIVATE ${TGEN})
    target_link_libraries(curate_push PRIVATE pmm_core fastdds fastcdr)
endif()

message(STATUS "pmm scaffold: crypto reuse + models + db (sqlite3) + smoke checks")
