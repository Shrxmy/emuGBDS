#include "emugbds/inds/Io.h"

#include "emugbds/inds/MemoryBus.h"

namespace emugbds::inds {
namespace {

constexpr uint32_t kDispStat = 0x04000004u;  // + VCOUNT in the upper half
constexpr uint32_t kDmaFirst = 0x040000B0u;
constexpr uint32_t kDmaLast = 0x040000DCu;
constexpr uint32_t kDmaFillFirst = 0x040000E0u;
constexpr uint32_t kDmaFillLast = 0x040000ECu;
constexpr uint32_t kTimerFirst = 0x04000100u;
constexpr uint32_t kTimerLast = 0x0400010Cu;
constexpr uint32_t kKeyInput = 0x04000130u;  // KEYINPUT | KEYCNT << 16
constexpr uint32_t kRcntExtKey = 0x04000134u; // RCNT | EXTKEYIN << 16 (ARM7)
constexpr uint32_t kIpcSync = 0x04000180u;
constexpr uint32_t kIpcFifoCnt = 0x04000184u;
constexpr uint32_t kIpcFifoSend = 0x04000188u;
constexpr uint32_t kIpcFifoRecv = 0x04100000u;
constexpr uint32_t kExmem = 0x04000204u;      // EXMEMCNT | WIFIWAITCNT << 16 (ARM7)
constexpr uint32_t kIme = 0x04000208u;
constexpr uint32_t kIe = 0x04000210u;
constexpr uint32_t kIf = 0x04000214u;
constexpr uint32_t kVramCntAD = 0x04000240u;  // ARM9 VRAMCNT_A-D; ARM7 VRAMSTAT | WRAMSTAT << 8
constexpr uint32_t kVramCntEG = 0x04000244u;  // ARM9 VRAMCNT_E-G | WRAMCNT << 24
constexpr uint32_t kVramCntHI = 0x04000248u;  // ARM9 VRAMCNT_H-I
constexpr uint32_t kPostFlg = 0x04000300u;    // POSTFLG | HALTCNT << 8 (ARM7)
constexpr uint32_t kPowCnt = 0x04000304u;

constexpr uint16_t kDispStatWritable = 0xFFB8u; // IRQ enables, LYC bit 8, LYC
constexpr uint16_t kKeysReleased = 0x03FFu;
// shortcut: no DS input is wired yet, so buttons, X/Y, and the pen read as released; hinge open.
constexpr uint16_t kExtKeysReleased = 0x007Fu;
constexpr uint32_t kDmaEnable = 1u << 31;

} // namespace

void Io::Reset() {
    cpus_ = {};
    for (CpuIo& cpu : cpus_) {
        cpu.timers.Reset();
    }
    cpus_[1].powcnt = 0x0001u; // POWCNT2: speakers on at power-up
    ipc_.Reset();
    dma_fill_ = {};
    vramcnt_ = {};
    exmem_shared_ = 0u;
    wifiwaitcnt_ = 0u;
    vcount_ = 0u;
    arm7_halt_requested_ = false;
    gba_mode_requested_ = false;
}


void Io::CollectIpcIrqs() noexcept {
    for (int cpu = 0; cpu < 2; ++cpu) {
        cpus_[cpu].iflags |= ipc_.TakeIrqs(cpu);
    }
}


bool Io::ReadWord(int cpu, uint32_t address, uint32_t mask, uint32_t& value) {
    CpuIo& self = cpus_[cpu];
    value = 0u;
    if (address >= kDmaFirst && address <= kDmaLast) {
        const DmaChannel& channel = self.dma[(address - kDmaFirst) / 12u];
        const uint32_t field = (address - kDmaFirst) % 12u;
        value = field == 0u ? channel.source : field == 4u ? channel.destination : channel.control;
        return true;
    }
    if (cpu == 0 && address >= kDmaFillFirst && address <= kDmaFillLast) {
        value = dma_fill_[(address - kDmaFillFirst) / 4u];
        return true;
    }
    if (address >= kTimerFirst && address <= kTimerLast) {
        value = self.timers.ReadWord(address - kTimerFirst);
        return true;
    }
    switch (address) {
    case kDispStat:
        value = self.dispstat | (static_cast<uint32_t>(vcount_) << 16u);
        return true;
    case kKeyInput:
        value = kKeysReleased | (static_cast<uint32_t>(self.keycnt) << 16u);
        return true;
    case kRcntExtKey:
        if (cpu != 1) {
            return false;
        }
        value = static_cast<uint32_t>(kExtKeysReleased) << 16u;
        return (mask & 0x0000FFFFu) == 0u; // RCNT (serial port) is not emulated.
    case kIpcSync:
        value = ipc_.ReadSync(cpu);
        return true;
    case kIpcFifoCnt:
        value = ipc_.ReadFifoControl(cpu);
        return true;
    case kIpcFifoSend:
        return true; // Write-only
    case kIpcFifoRecv:
        value = ipc_.Receive(cpu);
        CollectIpcIrqs();
        return true;
    case kExmem:
        value = (cpu == 0 ? 0u : static_cast<uint32_t>(wifiwaitcnt_) << 16u) |
            self.exmem_local | exmem_shared_ | (1u << 13);
        return true;
    case kIme:
        value = self.ime;
        return true;
    case kIe:
        value = self.ie;
        return true;
    case kIf:
        value = self.iflags;
        return true;
    case kVramCntAD:
        value = cpu == 0 ? 0u : static_cast<uint32_t>(memory_.wramcnt()) << 8u; // VRAMCNT is write-only
        return true;
    case kVramCntEG:
        if (cpu != 0) {
            return false;
        }
        value = static_cast<uint32_t>(memory_.wramcnt()) << 24u;
        return true;
    case kVramCntHI:
        return cpu == 0;
    case kPostFlg:
        value = self.postflg;
        return true;
    case kPowCnt:
        value = self.powcnt;
        return true;
    default:
        return false;
    }
}


bool Io::WriteWord(int cpu, uint32_t address, uint32_t value, uint32_t mask) {
    CpuIo& self = cpus_[cpu];
    if (address >= kDmaFirst && address <= kDmaLast) {
        WriteDma(cpu, (address - kDmaFirst) / 12u, (address - kDmaFirst) % 12u, value, mask);
        return true;
    }
    if (cpu == 0 && address >= kDmaFillFirst && address <= kDmaFillLast) {
        uint32_t& fill = dma_fill_[(address - kDmaFillFirst) / 4u];
        fill = Merge(fill, value, mask);
        return true;
    }
    if (address >= kTimerFirst && address <= kTimerLast) {
        self.timers.WriteWord(address - kTimerFirst, value, mask);
        return true;
    }
    switch (address) {
    case kDispStat:
        self.dispstat = static_cast<uint16_t>(Merge(self.dispstat, value, mask & kDispStatWritable));
        UpdateVCountMatch(cpu);
        return true; // VCOUNT writes (for linked consoles) are ignored.
    case kKeyInput:
        self.keycnt = static_cast<uint16_t>(Merge(self.keycnt, value >> 16u, (mask >> 16u) & 0xC3FFu));
        return true;
    case kIpcSync:
        ipc_.WriteSync(cpu, value, mask);
        CollectIpcIrqs();
        return true;
    case kIpcFifoCnt:
        ipc_.WriteFifoControl(cpu, value, mask);
        CollectIpcIrqs();
        return true;
    case kIpcFifoSend:
        ipc_.Send(cpu, value);
        CollectIpcIrqs();
        return true;
    case kExmem:
        self.exmem_local = static_cast<uint16_t>(Merge(self.exmem_local, value, mask & 0x007Fu));
        if (cpu == 0) {
            exmem_shared_ = static_cast<uint16_t>(Merge(exmem_shared_, value, mask & 0xC880u));
        } else {
            wifiwaitcnt_ = static_cast<uint16_t>(Merge(wifiwaitcnt_, value >> 16u, (mask >> 16u) & 0x003Fu));
        }
        return true;
    case kIme:
        self.ime = Merge(self.ime, value, mask & 1u);
        return true;
    case kIe:
        self.ie = Merge(self.ie, value, mask);
        return true;
    case kIf:
        self.iflags &= ~(value & mask); // Write 1 to acknowledge
        return true;
    case kVramCntAD:
    case kVramCntEG:
    case kVramCntHI:
        if (cpu != 0) {
            return address == kVramCntAD && (mask & 0xFFFFu) == 0u; // ARM7 VRAMSTAT/WRAMSTAT are read-only
        }
        for (uint32_t lane = 0u; lane < 4u; ++lane) {
            if ((mask & (0xFFu << (lane * 8u))) == 0u) {
                continue;
            }
            const uint8_t byte = static_cast<uint8_t>(value >> (lane * 8u));
            const uint32_t index = (address - kVramCntAD) + lane;
            if (index == 7u) {
                memory_.set_wramcnt(byte); // 4000247h WRAMCNT
            } else if (index < vramcnt_.size()) {
                vramcnt_[index] = byte;
            }
        }
        return true;
    case kPostFlg:
        if ((mask & 0xFFu) != 0u) {
            // Bit 0 cannot be cleared once set; bit 1 is plain R/W on the ARM9 only.
            self.postflg = static_cast<uint8_t>((self.postflg & 1u) | (value & (cpu == 0 ? 3u : 1u)));
        }
        if (cpu == 1 && (mask & 0xFF00u) != 0u) {
            switch ((value >> 14u) & 3u) { // HALTCNT bits 6-7
            case 1u: gba_mode_requested_ = true; break;
            case 2u:
            case 3u: arm7_halt_requested_ = true; break; // shortcut: Sleep behaves like Halt.
            default: break;
            }
        }
        return true;
    case kPowCnt:
        self.powcnt = static_cast<uint16_t>(Merge(self.powcnt, value, mask & (cpu == 0 ? 0x820Fu : 0x0003u)));
        return true;
    default:
        return false;
    }
}


void Io::AdvanceTimers(uint32_t cycles) {
    for (CpuIo& cpu : cpus_) {
        cpu.iflags |= cpu.timers.Advance(cycles);
    }
}


void Io::UpdateVCountMatch(int cpu) noexcept {
    CpuIo& self = cpus_[cpu];
    const uint16_t lyc = static_cast<uint16_t>((self.dispstat >> 8u) | ((self.dispstat & 0x0080u) << 1u));
    const bool match = vcount_ == lyc;
    if (match && (self.dispstat & kIrqVCount) == 0u && (self.dispstat & 0x0020u) != 0u) {
        self.iflags |= kIrqVCount;
    }
    self.dispstat = static_cast<uint16_t>((self.dispstat & ~kIrqVCount) | (match ? kIrqVCount : 0u));
}


void Io::StartLine(uint16_t vcount) {
    vcount_ = vcount;
    const bool vblank = vcount >= kVisibleLines && vcount < kTotalLines - 1u; // Flag clears on the last line.
    for (int cpu = 0; cpu < 2; ++cpu) {
        CpuIo& self = cpus_[cpu];
        self.dispstat = static_cast<uint16_t>((self.dispstat & ~0x0003u) | (vblank ? 0x0001u : 0u));
        UpdateVCountMatch(cpu); // Before V-Blank DMA, which may read DISPSTAT.
        if (vcount == kVisibleLines) {
            if ((self.dispstat & 0x0008u) != 0u) {
                self.iflags |= kIrqVBlank;
            }
            TriggerDma(cpu, 1u); // V-Blank DMA on both CPUs
        }
    }
}


void Io::StartHBlank() {
    for (int cpu = 0; cpu < 2; ++cpu) {
        CpuIo& self = cpus_[cpu];
        self.dispstat |= 0x0002u;
        if ((self.dispstat & 0x0010u) != 0u) {
            self.iflags |= kIrqHBlank;
        }
    }
    if (vcount_ < kVisibleLines) {
        TriggerDma(0, 2u); // ARM9 H-Blank DMA, paused during V-Blank
    }
}


uint32_t Io::DmaStartMode(int cpu, uint32_t control) const noexcept {
    return cpu == 0 ? (control >> 27u) & 7u : (control >> 28u) & 3u;
}


void Io::WriteDma(int cpu, uint32_t channel_index, uint32_t field, uint32_t value, uint32_t mask) {
    DmaChannel& channel = cpus_[cpu].dma[channel_index];
    if (field == 0u) {
        channel.source = Merge(channel.source, value, mask);
        return;
    }
    if (field == 4u) {
        channel.destination = Merge(channel.destination, value, mask);
        return;
    }
    const bool was_enabled = (channel.control & kDmaEnable) != 0u;
    channel.control = Merge(channel.control, value, mask);
    if (was_enabled || (channel.control & kDmaEnable) == 0u) {
        return;
    }
    // Enabling latches SAD, DAD, and the word count into internal counters.
    const uint32_t count_bits = cpu == 0 ? 0x1FFFFFu : channel_index == 3u ? 0xFFFFu : 0x3FFFu;
    channel.internal_source = channel.source;
    channel.internal_destination = channel.destination;
    channel.internal_count = channel.control & count_bits;
    if (DmaStartMode(cpu, channel.control) == 0u) {
        RunDma(cpu, channel_index);
    }
}


void Io::RunDma(int cpu, uint32_t channel_index) {
    DmaChannel& channel = cpus_[cpu].dma[channel_index];
    const uint32_t control = channel.control;
    const uint32_t unit = (control & (1u << 26)) != 0u ? 4u : 2u;
    const uint32_t count_bits = cpu == 0 ? 0x1FFFFFu : channel_index == 3u ? 0xFFFFu : 0x3FFFu;
    uint32_t units = channel.internal_count;
    if (units == 0u) {
        units = count_bits + 1u; // Zero means the maximum length.
    }
    const auto step = [unit](uint32_t mode) -> int32_t {
        return mode == 1u ? -static_cast<int32_t>(unit) : mode == 2u ? 0 : static_cast<int32_t>(unit);
    };
    const uint32_t destination_mode = (control >> 21u) & 3u;
    const int32_t destination_step = step(destination_mode);
    const int32_t source_step = step((control >> 23u) & 3u);
    const bool arm9 = cpu == 0;
    for (uint32_t i = 0u; i < units; ++i) {
        const uint32_t data = memory_.DmaRead(arm9, channel.internal_source & ~(unit - 1u), unit);
        memory_.DmaWrite(arm9, channel.internal_destination & ~(unit - 1u), data, unit);
        channel.internal_source += static_cast<uint32_t>(source_step);
        channel.internal_destination += static_cast<uint32_t>(destination_step);
    }
    if ((control & (1u << 30)) != 0u) {
        cpus_[cpu].iflags |= 1u << (8u + channel_index);
    }
    const bool repeat = (control & (1u << 25)) != 0u && DmaStartMode(cpu, control) != 0u;
    if (repeat) {
        channel.internal_count = control & count_bits;
        if (destination_mode == 3u) {
            channel.internal_destination = channel.destination;
        }
    } else {
        channel.control &= ~kDmaEnable;
    }
}


void Io::TriggerDma(int cpu, uint32_t mode) {
    for (uint32_t channel = 0u; channel < 4u; ++channel) {
        const uint32_t control = cpus_[cpu].dma[channel].control;
        if ((control & kDmaEnable) != 0u && DmaStartMode(cpu, control) == mode) {
            RunDma(cpu, channel);
        }
    }
}

} // namespace emugbds::inds
