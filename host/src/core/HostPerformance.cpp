#include "emugbds/core/HostPerformance.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <vector>
#endif

namespace emugbds::core {

void PreferPerformanceCores() noexcept {
#ifdef _WIN32
    PROCESS_POWER_THROTTLING_STATE throttling{};
    throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    throttling.StateMask = 0; // never throttle execution speed
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));

    // Hybrid CPUs report several efficiency classes; the highest one is the performance cores.
    ULONG length = 0;
    GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
    if (length == 0) {
        return;
    }
    std::vector<unsigned char> buffer(length);
    auto* first = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buffer.data());
    if (!GetSystemCpuSetInformation(first, length, &length, GetCurrentProcess(), 0)) {
        return;
    }
    BYTE lowest = 0xFF;
    BYTE highest = 0;
    for (ULONG offset = 0; offset < length;) {
        const auto* entry = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
        if (entry->Type == CpuSetInformation) {
            lowest = std::min(lowest, entry->CpuSet.EfficiencyClass);
            highest = std::max(highest, entry->CpuSet.EfficiencyClass);
        }
        offset += entry->Size;
    }
    if (lowest == highest) {
        return; // every core is alike
    }
    std::vector<ULONG> fast;
    for (ULONG offset = 0; offset < length;) {
        const auto* entry = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
        if (entry->Type == CpuSetInformation && entry->CpuSet.EfficiencyClass == highest) {
            fast.push_back(entry->CpuSet.Id);
        }
        offset += entry->Size;
    }
    if (!fast.empty()) {
        SetThreadSelectedCpuSets(GetCurrentThread(), fast.data(), static_cast<ULONG>(fast.size()));
    }
#endif
}

} // namespace emugbds::core
