#include "emugbds/ingba/Core.h"
#include "emugbds/ingba/CpuConstants.h"
#include "emugbds/ingba/DebugTrace.h"
#include "emugbds/ingba/MemoryConstants.h"
#include "emugbds/ingba/VideoConstants.h"
#include <cmath>

namespace emugbds::ingba {

bool Core::ExecuteSoftwareInterrupt(uint8_t swi_imm, uint32_t next_pc, bool from_thumb, std::string& error) {
    if (bus_.HasBios()) {
        // The BIOS SWI dispatcher extracts a Thumb SWI immediate from
        // [SVC_LR - 2]. For a Thumb SWI, SVC LR is the ordinary next
        // instruction address (the restored CPSR supplies the Thumb state),
        // not an address with bit 0 set.
        const uint32_t return_address = next_pc;
        EnterException(kCpuModeSupervisor, kSwiVectorAddress, false, return_address, true);
        return true;
    }

    const uint32_t saved_cpsr = cpu_.cpsr;
    WriteSpsrForMode(kCpuModeSupervisor, saved_cpsr);
    uint32_t svc_cpsr = (saved_cpsr & ~kCpuModeMask) | kCpuModeSupervisor;
    svc_cpsr |= kCpsrIrqDisableBit;
    svc_cpsr &= ~kCpsrThumbBit;
    WriteCpsr(svc_cpsr);
    cpu_.r[14] = next_pc | (from_thumb ? 1u : 0u);

    const bool handled = HandleSoftwareInterrupt(swi_imm, error);

    WriteCpsr(saved_cpsr);
    SetThumbState(from_thumb);
    const uint32_t old_pc = cpu_.r[15];
    cpu_.r[15] = next_pc & (from_thumb ? ~1u : ~3u);
    RecordPcFlowEvent("SWI.RETURN", old_pc, cpu_.r[15], static_cast<uint32_t>(swi_imm), cpu_.cpsr, cpu_.r[14], cpu_.r[13], frame_counter_);
    return handled;
}


bool Core::HandleSoftwareInterrupt(uint8_t swi_imm, std::string& error) {
    switch (swi_imm) {
    case 0x00: // SoftReset
        if (!Reset(error)) {
            return false;
        }
        cpu_.r[15] = rom_loaded_ ? kRomBase : kBiosBase;
        return true;
    case 0x01: { // RegisterRamReset
        const uint32_t flags = cpu_.r[0] & 0xFFu;
        if ((flags & 0x01u) != 0u) {
            for (uint32_t offset = 0u; offset < 0x40000u; offset += 4u) {
                bus_.Write32(kEwramBase + offset, 0u);
            }
        }
        if ((flags & 0x02u) != 0u) {
            for (uint32_t offset = 0u; offset < 0x8000u; offset += 4u) {
                bus_.Write32(kIwramBase + offset, 0u);
            }
        }
        if ((flags & 0x04u) != 0u) {
            for (uint32_t offset = 0u; offset < 0x400u; offset += 4u) {
                bus_.Write32(kPaletteBase + offset, 0u);
            }
        }
        if ((flags & 0x08u) != 0u) {
            for (uint32_t offset = 0u; offset < 0x18000u; offset += 4u) {
                bus_.Write32(kVramBase + offset, 0u);
            }
        }
        if ((flags & 0x10u) != 0u) {
            for (uint32_t offset = 0u; offset < 0x400u; offset += 4u) {
                bus_.Write32(kOamBase + offset, 0u);
            }
        }
        if ((flags & 0x20u) != 0u || (flags & 0x40u) != 0u || (flags & 0x80u) != 0u) {
            for (uint32_t offset = 0u; offset < 0x400u; offset += 4u) {
                bus_.Write32(kIoBase + offset, 0u);
            }
        }
        return true;
    }
    case 0x02: // Halt
    case 0x03: // Stop/Sleep
        halted_ = true;
    case 0x19: // SoundBias
    case 0x2A: // SoundDriverJmpTableCopy
        return true;
    case 0x0D: // GetBiosChecksum
        cpu_.r[0] = 0xBAAE187Fu;
        return true;
    case 0x0E: // BgAffineSet
        return ExecuteBgAffineSet();
    case 0x04: // IntrWait
        return ExecuteIntrWait(false, error);
    case 0x05: // VBlankIntrWait
        return ExecuteIntrWait(true, error);
    case 0x06: { // Div
        const int64_t numerator = static_cast<int64_t>(static_cast<int32_t>(cpu_.r[0]));
        const int64_t denominator = static_cast<int64_t>(static_cast<int32_t>(cpu_.r[1]));
        if (denominator == 0) {
            cpu_.r[0] = 0u;
            cpu_.r[1] = static_cast<uint32_t>(numerator);
            cpu_.r[3] = 0u;
            return true;
        }
        const int64_t quotient = numerator / denominator;
        const int64_t remainder = numerator % denominator;
        cpu_.r[0] = static_cast<uint32_t>(quotient);
        cpu_.r[1] = static_cast<uint32_t>(remainder);
        const int64_t abs_quotient = quotient < 0 ? -quotient : quotient;
        cpu_.r[3] = static_cast<uint32_t>(abs_quotient);
        return true;
    }
    case 0x07: { // DivArm (r0=denominator, r1=numerator)
        const int64_t denominator = static_cast<int64_t>(static_cast<int32_t>(cpu_.r[0]));
        const int64_t numerator = static_cast<int64_t>(static_cast<int32_t>(cpu_.r[1]));
        if (denominator == 0) {
            cpu_.r[0] = 0u;
            cpu_.r[1] = static_cast<uint32_t>(numerator);
            cpu_.r[3] = 0u;
            return true;
        }
        const int64_t quotient = numerator / denominator;
        const int64_t remainder = numerator % denominator;
        cpu_.r[0] = static_cast<uint32_t>(quotient);
        cpu_.r[1] = static_cast<uint32_t>(remainder);
        const int64_t abs_quotient = quotient < 0 ? -quotient : quotient;
        cpu_.r[3] = static_cast<uint32_t>(abs_quotient);
        return true;
    }
    case 0x08: { // Sqrt
        const double root = std::sqrt(static_cast<double>(cpu_.r[0]));
        cpu_.r[0] = static_cast<uint32_t>(root);
        return true;
    }
    case 0x0B: // CpuSet
        return ExecuteCpuSet(false);
    case 0x0C: // CpuFastSet
        return ExecuteCpuSet(true);
    case 0x11: // LZ77UnCompWram
        return ExecuteLz77UnComp(false, error);
    case 0x12: // LZ77UnCompVram
        return ExecuteLz77UnComp(true, error);
    case 0x14: // RLUnCompWram
        return ExecuteRlUnComp(false, error);
    case 0x15: // RLUnCompVram
        return ExecuteRlUnComp(true, error);
    default:
        last_unsupported_swi_ = swi_imm;
        error = "Unsupported GBA BIOS SWI: " + std::to_string(static_cast<unsigned>(swi_imm));
        return false;
    }
}


bool Core::ExecuteCpuSet(bool fast) {
    const uint32_t source = cpu_.r[0];
    const uint32_t destination = cpu_.r[1];
    const uint32_t control = cpu_.r[2];
    const bool fill = (control & (1u << 24)) != 0u;

    if (fast) {
        const uint32_t units = control & 0x001FFFFFu;
        const uint32_t words = units * 8u;
        if (words == 0u) {
            return true;
        }

        const uint32_t src_base = source & ~3u;
        const uint32_t dst_base = destination & ~3u;
        if (fill) {
            const uint32_t value = bus_.Read32(src_base);
            for (uint32_t i = 0; i < words; ++i) {
                bus_.Write32(dst_base + (i * 4u), value);
            }
            return true;
        }

        for (uint32_t i = 0; i < words; ++i) {
            const uint32_t value = bus_.Read32(src_base + (i * 4u));
            bus_.Write32(dst_base + (i * 4u), value);
        }
        return true;
    }

    const bool word_copy = (control & (1u << 26)) != 0u;
    const uint32_t count = control & 0x001FFFFFu;
    if (count == 0u) {
        return true;
    }

    if (word_copy) {
        const uint32_t src_base = source & ~3u;
        const uint32_t dst_base = destination & ~3u;
        if (fill) {
            const uint32_t value = bus_.Read32(src_base);
            for (uint32_t i = 0; i < count; ++i) {
                bus_.Write32(dst_base + (i * 4u), value);
            }
            return true;
        }

        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t value = bus_.Read32(src_base + (i * 4u));
            bus_.Write32(dst_base + (i * 4u), value);
        }
        return true;
    }

    const uint32_t src_base = source & ~1u;
    const uint32_t dst_base = destination & ~1u;
    if (fill) {
        const uint16_t value = bus_.Read16(src_base);
        for (uint32_t i = 0; i < count; ++i) {
            bus_.Write16(dst_base + (i * 2u), value);
        }
        return true;
    }

    for (uint32_t i = 0; i < count; ++i) {
        const uint16_t value = bus_.Read16(src_base + (i * 2u));
        bus_.Write16(dst_base + (i * 2u), value);
    }
    return true;
}


bool Core::ExecuteIntrWait(bool force_vblank, std::string& error) {
    (void)error;
    if (force_vblank) {
        cpu_.r[0] = 1u;
        cpu_.r[1] = 1u;
    }

    uint16_t mask = static_cast<uint16_t>(cpu_.r[1] & 0x3FFFu);
    if (mask == 0u) {
        mask = 0x0001u;
    }
    const bool discard_old = (cpu_.r[0] & 0x1u) != 0u;

    uint16_t iflags = bus_.Read16(kInterruptFlagAddress);
    if (discard_old) {
        bus_.AcknowledgeInterruptFlags(mask);
        iflags = bus_.Read16(kInterruptFlagAddress);
    }

    if ((iflags & mask) == 0u) {
        for (uint32_t i = 0u; i < (kCyclesPerFrame * 2u); ++i) {
            lcd_cycle_in_frame_ = (lcd_cycle_in_frame_ + 1u) % kCyclesPerFrame;
            bus_.AdvanceTimers(1u);
            AdvanceLcdTiming(lcd_cycle_in_frame_);
            iflags = bus_.Read16(kInterruptFlagAddress);
            if ((iflags & mask) != 0u) {
                break;
            }
        }
    }

    const uint16_t matched = static_cast<uint16_t>(iflags & mask);
    if (matched != 0u) {
        const uint16_t bios_mirror = bus_.Read16(kBiosIrqFlagsMirrorAddress);
        bus_.Write16(kBiosIrqFlagsMirrorAddress, static_cast<uint16_t>(bios_mirror | matched));
        const uint16_t iwram_mirror = bus_.Read16(kBiosIrqFlagsMirrorIwramAddress);
        bus_.Write16(kBiosIrqFlagsMirrorIwramAddress, static_cast<uint16_t>(iwram_mirror | matched));
        bus_.AcknowledgeInterruptFlags(matched);
    }
    return true;
}


bool Core::ExecuteBgAffineSet() {
    const uint32_t source = cpu_.r[0];
    const uint32_t destination = cpu_.r[1];
    const uint32_t count = cpu_.r[2];

    constexpr double kTwoPi = 6.28318530717958647692;

    for (uint32_t i = 0u; i < count; ++i) {
        const uint32_t src = source + (i * 20u);
        const int32_t tex_x = static_cast<int32_t>(bus_.Read32(src + 0u));
        const int32_t tex_y = static_cast<int32_t>(bus_.Read32(src + 4u));
        const int16_t disp_x = static_cast<int16_t>(bus_.Read16(src + 8u));
        const int16_t disp_y = static_cast<int16_t>(bus_.Read16(src + 10u));
        const int16_t scale_x_raw = static_cast<int16_t>(bus_.Read16(src + 12u));
        const int16_t scale_y_raw = static_cast<int16_t>(bus_.Read16(src + 14u));
        const uint16_t angle_raw = bus_.Read16(src + 16u);

        const double scale_x = static_cast<double>(scale_x_raw) / 256.0;
        const double scale_y = static_cast<double>(scale_y_raw) / 256.0;
        const double angle = (static_cast<double>(angle_raw) / 65536.0) * kTwoPi;
        const double cos_a = std::cos(angle);
        const double sin_a = std::sin(angle);

        const int16_t pa = static_cast<int16_t>(std::lround(cos_a * scale_x * 256.0));
        const int16_t pb = static_cast<int16_t>(std::lround(-sin_a * scale_x * 256.0));
        const int16_t pc = static_cast<int16_t>(std::lround(sin_a * scale_y * 256.0));
        const int16_t pd = static_cast<int16_t>(std::lround(cos_a * scale_y * 256.0));

        const int32_t start_x = tex_x -
            static_cast<int32_t>(pa) * static_cast<int32_t>(disp_x) -
            static_cast<int32_t>(pb) * static_cast<int32_t>(disp_y);
        const int32_t start_y = tex_y -
            static_cast<int32_t>(pc) * static_cast<int32_t>(disp_x) -
            static_cast<int32_t>(pd) * static_cast<int32_t>(disp_y);

        const uint32_t dst = destination + (i * 16u);
        bus_.Write16(dst + 0u, static_cast<uint16_t>(pa));
        bus_.Write16(dst + 2u, static_cast<uint16_t>(pb));
        bus_.Write16(dst + 4u, static_cast<uint16_t>(pc));
        bus_.Write16(dst + 6u, static_cast<uint16_t>(pd));
        bus_.Write32(dst + 8u, static_cast<uint32_t>(start_x));
        bus_.Write32(dst + 12u, static_cast<uint32_t>(start_y));
    }
    return true;
}


bool Core::ExecuteLz77UnComp(bool vram_mode, std::string& error) {
    const uint32_t source = cpu_.r[0];
    const uint32_t destination = cpu_.r[1];
    const uint32_t header = bus_.Read32(source);
    if ((header & 0xFFu) != 0x10u) {
        error = "Invalid LZ77 header type in SWI";
        return false;
    }
    const uint32_t output_size = header >> 8u;
    if (output_size == 0u) {
        return true;
    }

    std::vector<uint8_t> output;
    output.reserve(output_size);
    uint32_t cursor = source + 4u;

    while (output.size() < output_size) {
        const uint8_t flags = bus_.Read8(cursor++);
        for (uint32_t bit = 0u; bit < 8u && output.size() < output_size; ++bit) {
            const bool compressed = (flags & (0x80u >> bit)) != 0u;
            if (!compressed) {
                output.push_back(bus_.Read8(cursor++));
                continue;
            }

            const uint8_t token_hi = bus_.Read8(cursor++);
            const uint8_t token_lo = bus_.Read8(cursor++);
            const uint32_t length = static_cast<uint32_t>((token_hi >> 4u) + 3u);
            const uint32_t displacement = ((static_cast<uint32_t>(token_hi & 0x0Fu) << 8u) | token_lo) + 1u;
            if (displacement > output.size()) {
                error = "Invalid LZ77 displacement in SWI";
                return false;
            }

            size_t copy_pos = output.size() - displacement;
            for (uint32_t i = 0u; i < length && output.size() < output_size; ++i) {
                const uint8_t value = output[copy_pos++];
                output.push_back(value);
            }
        }
    }

    if (!vram_mode) {
        for (uint32_t i = 0u; i < output_size; ++i) {
            bus_.Write8(destination + i, output[i]);
        }
        return true;
    }

    uint32_t out_addr = destination & ~1u;
    for (uint32_t i = 0u; i < output_size; i += 2u) {
        const uint16_t lo = output[i];
        const uint16_t hi = (i + 1u < output_size) ? output[i + 1u] : 0u;
        bus_.Write16(out_addr, static_cast<uint16_t>(lo | (hi << 8u)));
        out_addr += 2u;
    }
    return true;
}


bool Core::ExecuteRlUnComp(bool vram_mode, std::string& error) {
    const uint32_t source = cpu_.r[0];
    const uint32_t destination = cpu_.r[1];
    const uint32_t header = bus_.Read32(source);
    if ((header & 0xFFu) != 0x30u) {
        error = "Invalid RL header type in SWI";
        return false;
    }
    const uint32_t output_size = header >> 8u;
    if (output_size == 0u) {
        return true;
    }

    std::vector<uint8_t> output;
    output.reserve(output_size);
    uint32_t cursor = source + 4u;

    while (output.size() < output_size) {
        const uint8_t control = bus_.Read8(cursor++);
        const bool compressed = (control & 0x80u) != 0u;
        const uint32_t count = static_cast<uint32_t>(control & 0x7Fu) + (compressed ? 3u : 1u);
        if (compressed) {
            const uint8_t value = bus_.Read8(cursor++);
            for (uint32_t i = 0u; i < count && output.size() < output_size; ++i) {
                output.push_back(value);
            }
            continue;
        }

        for (uint32_t i = 0u; i < count && output.size() < output_size; ++i) {
            output.push_back(bus_.Read8(cursor++));
        }
    }

    if (!vram_mode) {
        for (uint32_t i = 0u; i < output_size; ++i) {
            bus_.Write8(destination + i, output[i]);
        }
        return true;
    }

    uint32_t out_addr = destination & ~1u;
    for (uint32_t i = 0u; i < output_size; i += 2u) {
        const uint16_t lo = output[i];
        const uint16_t hi = (i + 1u < output_size) ? output[i + 1u] : 0u;
        bus_.Write16(out_addr, static_cast<uint16_t>(lo | (hi << 8u)));
        out_addr += 2u;
    }
    return true;
}


} // namespace emugbds::ingba
