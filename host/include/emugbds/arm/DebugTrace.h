#pragma once

// Optional diagnostics for the in-house ARM CPUs. Kept separate from runtime behavior.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace emugbds::arm {

#ifndef NDEBUG
struct PcFlowEvent {
    const char* site = "";
    uint32_t old_pc = 0u;
    uint32_t new_pc = 0u;
    uint32_t instruction = 0u;
    uint32_t cpsr = 0u;
    uint32_t lr = 0u;
    uint32_t sp = 0u;
    uint64_t frame = 0u;
};

inline constexpr size_t kPcFlowTraceDepth = 16u;
inline std::array<PcFlowEvent, kPcFlowTraceDepth> g_pc_flow_trace{};
inline size_t g_pc_flow_next_index = 0u;
inline size_t g_pc_flow_count = 0u;
inline bool g_pc_escape_reported = false;
inline bool g_irq_entry_reported = false;
inline bool g_irq_return_reported = false;

inline bool IsOpenBusPc(uint32_t pc) noexcept {
    return pc >= 0x00004000u && pc < 0x02000000u;
}

inline void ResetPcDebugTrace() noexcept {
    g_pc_flow_trace = {};
    g_pc_flow_next_index = 0u;
    g_pc_flow_count = 0u;
    g_pc_escape_reported = false;
    g_irq_entry_reported = false;
    g_irq_return_reported = false;
}

inline void RecordPcFlowEvent(
    const char* site,
    uint32_t old_pc,
    uint32_t new_pc,
    uint32_t instruction,
    uint32_t cpsr,
    uint32_t lr,
    uint32_t sp,
    uint64_t frame) noexcept {
    g_pc_flow_trace[g_pc_flow_next_index] = {site, old_pc, new_pc, instruction, cpsr, lr, sp, frame};
    g_pc_flow_next_index = (g_pc_flow_next_index + 1u) % kPcFlowTraceDepth;
    if (g_pc_flow_count < kPcFlowTraceDepth) {
        ++g_pc_flow_count;
    }
}

inline void DumpPcFlowTrace() noexcept {
    std::fprintf(stderr, "[TRACE] Last %zu PC flow event(s):\n", g_pc_flow_count);
    if (g_pc_flow_count == 0u) {
        return;
    }

    const size_t begin = (g_pc_flow_next_index + kPcFlowTraceDepth - g_pc_flow_count) % kPcFlowTraceDepth;
    for (size_t i = 0u; i < g_pc_flow_count; ++i) {
        const size_t idx = (begin + i) % kPcFlowTraceDepth;
        const PcFlowEvent& ev = g_pc_flow_trace[idx];
        std::fprintf(stderr,
            "  [%02zu] %-14s frame=%llu old=0x%08X new=0x%08X instr=0x%08X cpsr=0x%08X lr=0x%08X sp=0x%08X\n",
            i,
            ev.site,
            static_cast<unsigned long long>(ev.frame),
            ev.old_pc,
            ev.new_pc,
            ev.instruction,
            ev.cpsr,
            ev.lr,
            ev.sp);
    }
}

inline void ReportFirstPcEscape(
    uint64_t frame,
    uint32_t pre_pc,
    uint32_t post_pc,
    uint32_t pre_instruction,
    uint32_t pre_cpsr,
    bool pre_thumb,
    uint32_t irq_cb,
    uint16_t ie,
    uint16_t ime,
    uint16_t iflags) noexcept {
    if (g_pc_escape_reported) {
        return;
    }
    g_pc_escape_reported = true;

    std::fprintf(stderr,
        "[TRACE] PC escaped to open bus!\n"
        "  frame         = %llu\n"
        "  pre_pc        = 0x%08X\n"
        "  pre_instr     = 0x%08X (%s)\n"
        "  pre_cpsr      = 0x%08X  mode=%02X  T=%d\n"
        "  post_pc       = 0x%08X\n"
        "  [0x03007FFC]  = 0x%08X\n"
        "  IE=0x%04X  IME=0x%04X  IF=0x%04X\n",
        static_cast<unsigned long long>(frame),
        pre_pc,
        pre_instruction,
        pre_thumb ? "Thumb" : "ARM",
        pre_cpsr,
        static_cast<unsigned>(pre_cpsr & 0x1Fu),
        static_cast<int>((pre_cpsr >> 5u) & 1u),
        post_pc,
        irq_cb,
        ie,
        ime,
        iflags);
    DumpPcFlowTrace();
}

inline void ReportFirstBiosIrqEntry(
    uint64_t frame,
    uint32_t pc,
    uint32_t return_address,
    uint16_t pending,
    uint32_t irq_callback_slot) noexcept {
    if (g_irq_entry_reported) {
        return;
    }
    g_irq_entry_reported = true;
    std::fprintf(stderr,
        "[TRACE] BIOS IRQ entry\n"
        "  frame         = %llu\n"
        "  pc            = 0x%08X\n"
        "  return_addr   = 0x%08X\n"
        "  pending(IE&IF)= 0x%04X\n"
        "  [0x03007FFC]  = 0x%08X\n",
        static_cast<unsigned long long>(frame),
        pc,
        return_address,
        pending,
        irq_callback_slot);
}

inline void ReportFirstIrqReturn(
    uint64_t frame,
    uint32_t trigger_pc,
    uint32_t trigger_instr,
    uint32_t resume_pc,
    uint32_t restored_cpsr,
    uint32_t lr_value) noexcept {
    if (g_irq_return_reported) {
        return;
    }
    g_irq_return_reported = true;
    std::fprintf(stderr,
        "[TRACE] IRQ return via SUBS/MOVS to PC\n"
        "  frame         = %llu\n"
        "  trigger_pc     = 0x%08X  (address of the SUBS/MOVS instruction)\n"
        "  trigger_instr  = 0x%08X\n"
        "  resume_pc      = 0x%08X\n"
        "  restored_cpsr  = 0x%08X (mode=%02X T=%d)\n"
        "  lr_value       = 0x%08X\n",
        static_cast<unsigned long long>(frame),
        trigger_pc,
        trigger_instr,
        resume_pc,
        restored_cpsr,
        static_cast<unsigned>(restored_cpsr & 0x1Fu),
        static_cast<int>((restored_cpsr >> 5u) & 1u),
        lr_value);
}
#else
inline bool IsOpenBusPc(uint32_t) noexcept { return false; }
inline void ResetPcDebugTrace() noexcept {}
inline void RecordPcFlowEvent(const char*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint64_t) noexcept {}
inline void DumpPcFlowTrace() noexcept {}
inline void ReportFirstPcEscape(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, bool, uint32_t, uint16_t, uint16_t, uint16_t) noexcept {}
inline void ReportFirstBiosIrqEntry(uint64_t, uint32_t, uint32_t, uint16_t, uint32_t) noexcept {}
inline void ReportFirstIrqReturn(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) noexcept {}
#endif

} // namespace emugbds::arm
