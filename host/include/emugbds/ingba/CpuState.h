#pragma once

#include <array>
#include <cstdint>

namespace emugbds::ingba {

struct CpuState {
    std::array<uint32_t, 16> r{};
    uint32_t cpsr = 0x0000001Fu;
    bool thumb = false;
    // R8-R12 are shared by User/System and the exception modes, but FIQ has
    // its own copies of them.
    std::array<uint32_t, 5> shared_r8_r12{};
    // User and System modes share the same banked SP/LR pair.
    std::array<uint32_t, 2> banked_r13_r14_usr{};
    std::array<uint32_t, 2> banked_r13_r14_irq{};
    std::array<uint32_t, 2> banked_r13_r14_svc{};
    std::array<uint32_t, 2> banked_r13_r14_abt{};
    std::array<uint32_t, 2> banked_r13_r14_und{};
    std::array<uint32_t, 7> banked_r8_r14_fiq{};
    uint32_t spsr_fiq = 0u;
    uint32_t spsr_irq = 0u;
    uint32_t spsr_svc = 0u;
    uint32_t spsr_abt = 0u;
    uint32_t spsr_und = 0u;
};

} // namespace emugbds::ingba
