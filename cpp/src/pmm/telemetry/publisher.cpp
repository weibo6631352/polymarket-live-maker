// pmm/telemetry/publisher.cpp — 默认 (无 Fast-DDS) 工厂: 返回 NoopPublisher。
//
// WITH_TELEMETRY=ON 时 CMake 改编 dds_publisher.cpp (提供 Fast-DDS 版 make_publisher), 不编本文件。
#include "pmm/telemetry/publisher.hpp"

namespace pmm::telemetry {

std::unique_ptr<Publisher> make_publisher() {
    return std::make_unique<NoopPublisher>();
}

}  // namespace pmm::telemetry
