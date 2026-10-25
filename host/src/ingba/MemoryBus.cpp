#include "emugbds/ingba/MemoryBus.h"
#include "emugbds/ingba/MemoryConstants.h"
#include "emugbds/ingba/VideoConstants.h"

#include <algorithm>
#include <limits>

namespace emugbds::ingba {
namespace {

// Timer prescalers 1/64/256/1024 as shifts; timers advance once per CPU step.
constexpr std::array<uint32_t, 4u> kPrescalerShifts = {0u, 6u, 8u, 10u};

} // namespace

bool MemoryBus::LoadBios(const std::filesystem::path& bios_path, std::string& error) {
    std::vector<uint8_t> bios_image;
    if (!LoadWholeFile(bios_path, bios_image, error)) {
        return false;
    }
    if (bios_image.size() != kBiosSize) {
        error = "GBA BIOS must be exactly 16 KiB: " + bios_path.string();
        return false;
    }

    bios_ = std::move(bios_image);
    bios_loaded_ = true;
    if (ewram_.empty()) {
        ewram_.assign(kEwramSize, 0);
    }
    if (iwram_.empty()) {
        iwram_.assign(kIwramSize, 0);
    }
    return true;
}


bool MemoryBus::LoadRom(const std::filesystem::path& rom_path, std::string& error) {
    std::vector<uint8_t> rom_image;
    if (!LoadWholeFile(rom_path, rom_image, error)) {
        return false;
    }

    std::filesystem::path save_path = rom_path;
    save_path.replace_extension(".sav");
    Backup backup; // the running game keeps its save chip if this load fails
    if (!backup.Load(Backup::Detect(rom_image), save_path, error)) {
        return false;
    }
    backup_ = std::move(backup);
    rom_ = std::move(rom_image);
    rtc_.Reset();
    gpio_data_ = 0u;
    gpio_direction_ = 0u;
    gpio_control_ = 0u;
    apu_.Reset();
    if (ewram_.empty()) {
        ewram_.assign(kEwramSize, 0);
    }
    if (iwram_.empty()) {
        iwram_.assign(kIwramSize, 0);
    }
    return true;
}


void MemoryBus::ApplyKeyInputMasks(uint32_t press_mask, uint32_t release_mask) noexcept {
    const uint16_t press = static_cast<uint16_t>(press_mask & 0x03FFu);
    const uint16_t release = static_cast<uint16_t>(release_mask & 0x03FFu);
    keyinput_state_ = static_cast<uint16_t>((keyinput_state_ & ~press) | release);
    keyinput_state_ &= 0x03FFu;
}


void MemoryBus::SetLcdStatus(uint16_t vcount, bool vblank, bool hblank, bool vcounter_match) noexcept {
    const uint32_t dispstat_lo = kDispStatAddress - kIoBase;
    const uint32_t dispstat_hi = dispstat_lo + 1u;
    const uint32_t vcount_lo = kVCountAddress - kIoBase;
    const uint32_t vcount_hi = vcount_lo + 1u;

    uint16_t dispstat = static_cast<uint16_t>(io_[dispstat_lo] | (static_cast<uint16_t>(io_[dispstat_hi]) << 8u));
    dispstat &= static_cast<uint16_t>(~(kDispStatVBlank | kDispStatHBlank | kDispStatVCounter));
    if (vblank) {
        dispstat |= kDispStatVBlank;
    }
    if (hblank) {
        dispstat |= kDispStatHBlank;
    }
    if (vcounter_match) {
        dispstat |= kDispStatVCounter;
    }

    io_[dispstat_lo] = static_cast<uint8_t>(dispstat & 0xFFu);
    io_[dispstat_hi] = static_cast<uint8_t>((dispstat >> 8u) & 0xFFu);
    io_[vcount_lo] = static_cast<uint8_t>(vcount & 0xFFu);
    io_[vcount_hi] = static_cast<uint8_t>((vcount >> 8u) & 0xFFu);
}


// IE & IF when IME allows delivery. The CPU polls this every step, so it reads the
// plain IO storage that Read8 would return for these registers.
uint16_t MemoryBus::PendingInterrupts() const noexcept {
    if ((io_[kInterruptMasterEnableAddress - kIoBase] & 0x01u) == 0u) {
        return 0u;
    }
    return static_cast<uint16_t>(
        ReadIo16Direct(kInterruptEnableAddress) & ReadIo16Direct(kInterruptFlagAddress));
}


uint16_t MemoryBus::ReadIo16Direct(uint32_t address) const {
    if (address < kIoBase || address + 1u >= kIoBase + kIoSize) {
        return 0u;
    }
    const uint32_t offset = address - kIoBase;
    return static_cast<uint16_t>(io_[offset] | (static_cast<uint16_t>(io_[offset + 1u]) << 8u));
}


uint32_t MemoryBus::ReadIo32Direct(uint32_t address) const {
    if (address < kIoBase || address + 3u >= kIoBase + kIoSize) {
        return 0u;
    }
    const uint32_t offset = address - kIoBase;
    return static_cast<uint32_t>(io_[offset]) |
        (static_cast<uint32_t>(io_[offset + 1u]) << 8u) |
        (static_cast<uint32_t>(io_[offset + 2u]) << 16u) |
        (static_cast<uint32_t>(io_[offset + 3u]) << 24u);
}


void MemoryBus::WriteIo16Direct(uint32_t address, uint16_t value) {
    if (address < kIoBase || address + 1u >= kIoBase + kIoSize) {
        return;
    }
    const uint32_t offset = address - kIoBase;
    io_[offset] = static_cast<uint8_t>(value & 0xFFu);
    io_[offset + 1u] = static_cast<uint8_t>((value >> 8u) & 0xFFu);
}


void MemoryBus::WriteIo32Direct(uint32_t address, uint32_t value) {
    if (address < kIoBase || address + 3u >= kIoBase + kIoSize) {
        return;
    }
    const uint32_t offset = address - kIoBase;
    io_[offset] = static_cast<uint8_t>(value & 0xFFu);
    io_[offset + 1u] = static_cast<uint8_t>((value >> 8u) & 0xFFu);
    io_[offset + 2u] = static_cast<uint8_t>((value >> 16u) & 0xFFu);
    io_[offset + 3u] = static_cast<uint8_t>((value >> 24u) & 0xFFu);
}


bool MemoryBus::DecodeTimerCountAddress(
    uint32_t address,
    uint32_t& timer,
    bool& high_byte) noexcept {
    if (address < kIoBase + 0x100u || address >= kIoBase + 0x110u) {
        return false;
    }

    const uint32_t offset = address - (kIoBase + 0x100u);
    timer = offset / 4u;
    high_byte = (offset & 0x2u) != 0u;
    return true;
}

void MemoryBus::IncrementTimer(uint32_t timer, uint32_t increments) noexcept {
    if (timer >= timer_counters_.size() || increments == 0u ||
        (timer_control_[timer] & 0x0080u) == 0u) {
        return;
    }

    uint32_t remaining = increments;
    while (remaining != 0u) {
        const uint32_t counter = timer_counters_[timer];
        const uint32_t until_overflow = 0x10000u - counter;
        if (remaining < until_overflow) {
            timer_counters_[timer] = static_cast<uint16_t>(counter + remaining);
            return;
        }

        remaining -= until_overflow;
        timer_counters_[timer] = timer_reload_[timer];
        if ((timer_control_[timer] & 0x0040u) != 0u) {
            SetInterruptRequest(static_cast<uint16_t>(1u << (3u + timer)));
        }
        if (timer < 2u) {
            const uint32_t refill = apu_.OnTimerOverflow(timer, ReadIo16Direct(kSoundCntHAddress));
            for (uint32_t fifo = 0u; fifo < 2u; ++fifo) {
                if ((refill & (1u << fifo)) != 0u) {
                    RequestSoundDma(fifo);
                }
            }
        }
        if (timer + 1u < timer_counters_.size() &&
            (timer_control_[timer + 1u] & 0x0084u) == 0x0084u) {
            IncrementTimer(timer + 1u, 1u);
        }
    }
}

void MemoryBus::AdvanceTimers(uint32_t cycles) {
    if (cycles == 0u) {
        return;
    }

    // Sample before the timers move: callers never step past a timer overflow,
    // so FIFO output levels are constant across these cycles.
    apu_.Advance(cycles, ReadIo16Direct(kSoundCntHAddress), ReadIo16Direct(kSoundCntXAddress));
    for (uint32_t timer = 0u; timer < timer_counters_.size(); ++timer) {
        const uint16_t control = timer_control_[timer];
        if ((control & 0x0080u) == 0u || (control & 0x0004u) != 0u) {
            continue;
        }

        const uint32_t shift = kPrescalerShifts[control & 0x3u];
        const uint64_t accumulated = static_cast<uint64_t>(timer_prescaler_phase_[timer]) + cycles;
        const uint32_t increments = static_cast<uint32_t>(accumulated >> shift);
        timer_prescaler_phase_[timer] = static_cast<uint32_t>(accumulated & ((1u << shift) - 1u));
        IncrementTimer(timer, increments);
    }
}

// Cascaded timers only tick when a lower timer overflows, so the earliest
// prescaled overflow bounds every timer event, IRQ or not.
uint32_t MemoryBus::CyclesUntilTimerOverflow() const noexcept {
    uint32_t earliest = std::numeric_limits<uint32_t>::max();
    for (uint32_t timer = 0u; timer < timer_counters_.size(); ++timer) {
        const uint16_t control = timer_control_[timer];
        if ((control & 0x0080u) == 0u || (control & 0x0004u) != 0u) {
            continue;
        }
        const uint32_t ticks = 0x10000u - timer_counters_[timer];
        const uint32_t cycles = (ticks << kPrescalerShifts[control & 0x3u]) - timer_prescaler_phase_[timer];
        earliest = std::min(earliest, cycles);
    }
    return earliest;
}

void MemoryBus::TryStartImmediateDma(uint32_t control_high_address, uint8_t previous_control_high) {
    for (uint32_t channel = 0; channel < kDmaControlHighByteAddresses.size(); ++channel) {
        if (kDmaControlHighByteAddresses[channel] != control_high_address) {
            continue;
        }
        const uint32_t base = kDmaBaseAddresses[channel];
        const uint16_t control = ReadIo16Direct(base + 10u);
        if ((control & kDmaEnableBit) == 0u || (previous_control_high & 0x80u) != 0u) {
            return; // Only an enable edge (0 -> 1) starts a channel.
        }
        // The hardware copies SAD/DAD/CNT_L into internal counters and never writes them back,
        // so sound engines can restart a buffer just by toggling the enable bit.
        dma_source_[channel] = ReadIo32Direct(base + 0u);
        dma_destination_[channel] = ReadIo32Direct(base + 4u);
        dma_count_[channel] = ReadIo16Direct(base + 8u);
        if ((control & kDmaStartTimingMask) != 0u) {
            return;
        }
        ExecuteDmaTransfer(channel, false);
        return;
    }
}


void MemoryBus::TriggerDmaTiming(uint32_t timing_code) {
    for (uint32_t channel = 0; channel < kDmaBaseAddresses.size(); ++channel) {
        const uint32_t control_address = kDmaBaseAddresses[channel] + 10u;
        const uint16_t control = ReadIo16Direct(control_address);
        if ((control & kDmaEnableBit) == 0u) {
            continue;
        }
        const uint32_t start_timing = (control >> 12u) & 0x3u;
        if (start_timing != timing_code || (timing_code == 0x3u && channel != 3u)) {
            continue;
        }
        ExecuteDmaTransfer(channel, true);
    }
}


// DMA1/DMA2 in "special" timing serve whichever FIFO their destination names.
void MemoryBus::RequestSoundDma(uint32_t fifo) {
    const uint32_t fifo_address = fifo == 0u ? kFifoAAddress : kFifoBAddress;
    for (uint32_t channel = 1u; channel <= 2u; ++channel) {
        const uint32_t base = kDmaBaseAddresses[channel];
        const uint16_t control = ReadIo16Direct(base + 10u);
        if ((control & kDmaEnableBit) != 0u && ((control >> 12u) & 0x3u) == 0x3u &&
            dma_destination_[channel] == fifo_address) { // FIFO mode never moves the latched destination.
            ExecuteDmaTransfer(channel, true);
            return;
        }
    }
}


void MemoryBus::SetInterruptRequest(uint16_t flags) noexcept {
    const uint16_t current = ReadIo16Direct(kInterruptFlagAddress);
    WriteIo16Direct(kInterruptFlagAddress, static_cast<uint16_t>(current | flags));
}

void MemoryBus::AcknowledgeInterruptFlags(uint16_t flags) noexcept {
    const uint16_t current = ReadIo16Direct(kInterruptFlagAddress);
    WriteIo16Direct(kInterruptFlagAddress, static_cast<uint16_t>(current & ~flags));
}


void MemoryBus::ExecuteDmaTransfer(uint32_t channel, bool timed_trigger) {
    if (channel >= kDmaBaseAddresses.size()) {
        return;
    }

    const uint32_t base = kDmaBaseAddresses[channel];
    uint32_t source = dma_source_[channel];
    uint32_t destination = dma_destination_[channel];

    const uint16_t count = dma_count_[channel];
    uint16_t control = ReadIo16Direct(base + 10u);

    // Sound FIFO mode always moves 4 words to a fixed destination, ignoring count and width.
    const bool fifo_mode = timed_trigger && (channel == 1u || channel == 2u) &&
        ((control >> 12u) & 0x3u) == 0x3u;
    const bool word_transfer = fifo_mode || (control & kDmaTransfer32Bit) != 0u;
    const uint32_t unit_size = word_transfer ? 4u : 2u;
    uint32_t units = fifo_mode ? 4u : count;
    if (units == 0u) {
        units = (channel == 3u) ? 0x10000u : 0x4000u;
    }

    const uint32_t destination_control = (control >> 5u) & 0x3u;
    const uint32_t source_control = (control >> 7u) & 0x3u;
    const bool destination_reload = destination_control == 0x3u;

    auto compute_delta = [&](uint32_t mode, bool source_mode) -> int32_t {
        switch (mode) {
        case 0x0u:
            return static_cast<int32_t>(unit_size);
        case 0x1u:
            return -static_cast<int32_t>(unit_size);
        case 0x2u:
            return 0;
        case 0x3u:
            if (source_mode) {
                return static_cast<int32_t>(unit_size);
            }
            return static_cast<int32_t>(unit_size);
        default:
            return static_cast<int32_t>(unit_size);
        }
    };

    if (channel == 3u && InEepromWindow(destination)) {
        backup_.BeginEepromStream(units); // the chip decodes the command once all its bits arrive
    }
    const int32_t source_delta = compute_delta(source_control, true);
    const int32_t destination_delta = fifo_mode ? 0 : compute_delta(destination_control, false);

    for (uint32_t unit = 0u; unit < units; ++unit) {
        if (word_transfer) {
            const uint32_t value = Read32(source & ~3u);
            Write32(destination & ~3u, value);
        } else {
            const uint16_t value = Read16(source & ~1u);
            Write16(destination & ~1u, value);
        }

        source = static_cast<uint32_t>(static_cast<int64_t>(source) + source_delta);
        destination = static_cast<uint32_t>(static_cast<int64_t>(destination) + destination_delta);
    }

    // Repeats reload CNT_L, and DAD only in increment/reload mode.
    const bool repeat = (control & kDmaRepeatBit) != 0u;
    dma_source_[channel] = source;
    dma_destination_[channel] = (destination_reload && repeat) ? ReadIo32Direct(base + 4u) : destination;
    if (repeat) {
        dma_count_[channel] = ReadIo16Direct(base + 8u);
    }
    if ((control & kDmaIrqBit) != 0u) {
        SetInterruptRequest(static_cast<uint16_t>(1u << (8u + channel)));
    }

    const bool keep_enabled = timed_trigger && (control & kDmaRepeatBit) != 0u &&
        ((control & kDmaStartTimingMask) != 0u);
    if (!keep_enabled) {
        control = static_cast<uint16_t>(control & ~kDmaEnableBit);
    }
    WriteIo16Direct(base + 10u, control);
}


uint8_t MemoryBus::Read8(uint32_t address) const {
    if (const uint8_t* bytes = DirectReadPointer(address, 1u)) {
        return *bytes;
    }
    if (address >= kBiosBase && address < kBiosBase + kBiosSize) {
        if (!bios_loaded_) {
            return 0;
        }
        return ReadFromSpan(bios_, kBiosBase, address);
    }
    // EWRAM and IWRAM are mirrored throughout their respective 16 MiB
    // address windows. The BIOS IRQ vector accesses the IWRAM callback slot
    // through the 0x03FFFFFC mirror, not only through 0x03007FFC.
    if (address >= kEwramBase && address < kEwramBase + 0x01000000u) {
        return ReadFromMirroredSpan(ewram_, kEwramBase, address);
    }
    if (address >= kIwramBase && address < kIwramBase + 0x01000000u) {
        return ReadFromMirroredSpan(iwram_, kIwramBase, address);
    }
    if (address >= kIoBase && address < kIoBase + kIoSize) {
        uint32_t timer = 0u;
        bool high_byte = false;
        if (DecodeTimerCountAddress(address, timer, high_byte)) {
            const uint32_t byte_in_register = (address - (kIoBase + 0x100u)) & 0x3u;
            if (!high_byte) {
                const uint16_t counter = timer_counters_[timer];
                return static_cast<uint8_t>((counter >> (byte_in_register * 8u)) & 0xFFu);
            }
            return byte_in_register == 2u
                ? static_cast<uint8_t>(timer_control_[timer] & 0x00FFu)
                : 0u;
        }
        if (address == kKeyInputAddress) {
            return static_cast<uint8_t>(keyinput_state_ & 0xFFu);
        }
        if (address == kKeyInputAddress + 1u) {
            return static_cast<uint8_t>((keyinput_state_ >> 8u) & 0xFFu);
        }
        if (address == kPostFlgAddress) {
            return postflg_;
        }
        if (address == kPostFlgAddress + 1u) {
            return 0u;
        }
        if (address == kWaitCntAddress) {
            return static_cast<uint8_t>(waitcnt_ & 0xFFu);
        }
        if (address == kWaitCntAddress + 1u) {
            return static_cast<uint8_t>((waitcnt_ >> 8u) & 0xFFu);
        }
        return io_[address - kIoBase];
    }
    if (address >= kPaletteBase && address < kPaletteBase + 0x01000000u) {
        return ReadFromMirroredArray(palette_, kPaletteBase, address);
    }
    if (address >= kVramBase && address < kVramBase + 0x01000000u) {
        return vram_[NormalizeVramOffset(address)];
    }
    if (address >= kOamBase && address < kOamBase + 0x01000000u) {
        return ReadFromMirroredArray(oam_, kOamBase, address);
    }
    if (address == kKeyInputAddress) {
        return static_cast<uint8_t>(keyinput_state_ & 0xFFu);
    }
    if (address == kKeyInputAddress + 1u) {
        return static_cast<uint8_t>((keyinput_state_ >> 8u) & 0xFFu);
    }
    if (address >= kSaveBase && address < kSaveEnd) {
        return backup_.Read8(address - kSaveBase);
    }
    if (InEepromWindow(address)) {
        return static_cast<uint8_t>(backup_.ReadEepromBit());
    }
    if (InGpio(address) && (gpio_control_ & 1u) != 0u) {
        return ReadGpio(address);
    }
    if (address >= kRomBase && address < kSaveBase && !rom_.empty()) {
        const uint32_t rom_offset = (address - kRomBase) % static_cast<uint32_t>(rom_.size());
        return rom_[rom_offset];
    }
    return 0;
}


// Side-effect-free regions are read in one go; the mirroring matches the byte-wise
// path in Read8. BIOS, IO and save memory return nullptr and take that path.
const uint8_t* MemoryBus::DirectReadPointer(uint32_t address, uint32_t size) const noexcept {
    if ((address & (size - 1u)) != 0u) {
        return nullptr; // Aligned accesses never straddle a mirror boundary.
    }
    const auto mirrored = [&](const std::vector<uint8_t>& bytes, uint32_t base) -> const uint8_t* {
        if (bytes.empty()) {
            return nullptr;
        }
        uint32_t offset = address - base;
        if (offset >= bytes.size()) {
            offset %= static_cast<uint32_t>(bytes.size()); // Only mirrored accesses pay for the division.
        }
        return offset + size <= bytes.size() ? bytes.data() + offset : nullptr;
    };
    if ((gpio_control_ & 1u) != 0u && address - 0x080000C0u < 0x10u) {
        return nullptr; // the readable GPIO registers sit inside this ROM block
    }
    switch (address >> 24u) {
    case 0x02u:
        return mirrored(ewram_, kEwramBase);
    case 0x03u:
        return mirrored(iwram_, kIwramBase);
    case 0x05u:
        return palette_.data() + ((address - kPaletteBase) % palette_.size());
    case 0x06u:
        return vram_.data() + NormalizeVramOffset(address);
    case 0x07u:
        return oam_.data() + ((address - kOamBase) % oam_.size());
    case 0x08u: case 0x09u: case 0x0Au: case 0x0Bu: case 0x0Cu:
        return mirrored(rom_, kRomBase);
    case 0x0Du:
        return InEepromWindow(address) ? nullptr : mirrored(rom_, kRomBase);
    default:
        return nullptr;
    }
}


// GBATEK "GBA Cart I/O Port (GPIO)": input bits of the data register come from the device (the RTC drives SIO).
uint8_t MemoryBus::ReadGpio(uint32_t address) const noexcept {
    if ((address & 1u) != 0u) {
        return 0u; // bits 4-15 read as zero
    }
    switch (address - 0x080000C4u) {
    case 0u: {
        const uint8_t from_rtc = rtc_.sio() ? CartRtc::kSio : 0u;
        return static_cast<uint8_t>(((gpio_data_ & gpio_direction_) | (from_rtc & ~gpio_direction_)) & 0x0Fu);
    }
    case 2u: return gpio_direction_;
    default: return gpio_control_;
    }
}


void MemoryBus::WriteGpio(uint32_t address, uint16_t value) noexcept {
    switch (address - 0x080000C4u) {
    case 0u: gpio_data_ = static_cast<uint8_t>(value & 0x0Fu); break;
    case 2u: gpio_direction_ = static_cast<uint8_t>(value & 0x0Fu); break;
    default: gpio_control_ = static_cast<uint8_t>(value & 1u); return;
    }
    rtc_.WritePins(static_cast<uint8_t>(gpio_data_ & gpio_direction_)); // only driven lines reach the RTC
}


uint16_t MemoryBus::Read16(uint32_t address) const {
    if (const uint8_t* bytes = DirectReadPointer(address, 2u)) {
        return static_cast<uint16_t>(bytes[0] | (bytes[1] << 8u));
    }
    if (InEepromWindow(address)) {
        return backup_.ReadEepromBit(); // one serial bit per halfword
    }
    if (address >= kSaveBase && address < kSaveEnd) {
        return static_cast<uint16_t>(Read8(address) * 0x0101u); // 8-bit bus: the byte appears in both halves
    }
    const uint16_t lo = Read8(address);
    const uint16_t hi = Read8(address + 1);
    return static_cast<uint16_t>(lo | (hi << 8));
}


uint32_t MemoryBus::Read32(uint32_t address) const {
    if (const uint8_t* bytes = DirectReadPointer(address, 4u)) {
        return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8u) |
            (static_cast<uint32_t>(bytes[2]) << 16u) | (static_cast<uint32_t>(bytes[3]) << 24u);
    }
    if (InEepromWindow(address)) {
        return backup_.ReadEepromBit();
    }
    if (address >= kSaveBase && address < kSaveEnd) {
        return Read8(address) * 0x01010101u;
    }
    const uint32_t b0 = Read8(address);
    const uint32_t b1 = Read8(address + 1);
    const uint32_t b2 = Read8(address + 2);
    const uint32_t b3 = Read8(address + 3);
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}


void MemoryBus::Write8(uint32_t address, uint8_t value) {
    if (address >= kEwramBase && address < kEwramBase + 0x01000000u) {
        WriteToMirroredSpan(ewram_, kEwramBase, address, value);
        return;
    }
    if (address >= kIwramBase && address < kIwramBase + 0x01000000u) {
        WriteToMirroredSpan(iwram_, kIwramBase, address, value);
        return;
    }
    if (address >= kIoBase && address < kIoBase + kIoSize) {
        uint32_t timer = 0u;
        bool high_byte = false;
        if (DecodeTimerCountAddress(address, timer, high_byte)) {
            const uint32_t byte_in_register = (address - (kIoBase + 0x100u)) & 0x3u;
            const uint32_t timer_base = kIoBase + 0x100u + (timer * 4u);
            if (!high_byte) {
                io_[address - kIoBase] = value;
                timer_reload_[timer] = ReadIo16Direct(timer_base);
            } else if (byte_in_register == 2u) {
                const bool was_enabled = (timer_control_[timer] & 0x0080u) != 0u;
                io_[address - kIoBase] = value;
                timer_control_[timer] = value;
                const bool is_enabled = (timer_control_[timer] & 0x0080u) != 0u;
                if (!was_enabled && is_enabled) {
                    timer_counters_[timer] = timer_reload_[timer];
                    timer_prescaler_phase_[timer] = 0u;
                } else if (was_enabled && !is_enabled) {
                    timer_prescaler_phase_[timer] = 0u;
                }
            }
            return;
        }
        if (address == kKeyInputAddress || address == kKeyInputAddress + 1u || address == kVCountAddress ||
            address == kVCountAddress + 1u) {
            return;
        }
        if (address == kPostFlgAddress + 1u) {
            // HALTCNT is write-only. Bit 7 distinguishes STOP from HALT;
            // both suspend instruction execution while LCD timing continues.
            halt_requested_ = true;
            return;
        }
        if (address == kInterruptFlagAddress || address == kInterruptFlagAddress + 1u) {
            const uint16_t current_if = ReadIo16Direct(kInterruptFlagAddress);
            const uint16_t clear_mask = (address == kInterruptFlagAddress)
                ? static_cast<uint16_t>(value)
                : static_cast<uint16_t>(static_cast<uint16_t>(value) << 8u);
            WriteIo16Direct(kInterruptFlagAddress, static_cast<uint16_t>(current_if & ~clear_mask));
            return;
        }

        if (address == kPostFlgAddress) {
            postflg_ = value;
            return;
        }
        if (address == kWaitCntAddress || address == kWaitCntAddress + 1u) {
            if (address == kWaitCntAddress) {
                waitcnt_ = static_cast<uint16_t>((waitcnt_ & 0xFF00u) | value);
            } else {
                waitcnt_ = static_cast<uint16_t>((waitcnt_ & 0x00FFu) | (static_cast<uint16_t>(value) << 8u));
            }
            io_[address - kIoBase] = value;
            return;
        }

        if (address >= kFifoAAddress && address < kFifoBAddress + 4u) {
            apu_.WriteFifo((address - kFifoAAddress) / 4u, value);
            return;
        }
        if (address == kSoundCntHAddress + 1u) {
            // Bits 11 and 15 reset FIFO A/B and are not stored.
            if ((value & 0x08u) != 0u) {
                apu_.ResetFifo(0u);
            }
            if ((value & 0x80u) != 0u) {
                apu_.ResetFifo(1u);
            }
            value = static_cast<uint8_t>(value & ~0x88u);
        }
        if (address == kDispStatAddress) {
            // DISPSTAT bits 0-2 are LCD status flags owned by the video timing, not the CPU.
            const uint8_t status_mask = kDispStatVBlank | kDispStatHBlank | kDispStatVCounter;
            value = static_cast<uint8_t>((value & ~status_mask) | (io_[address - kIoBase] & status_mask));
        } else if (address == kDispStatAddress + 1u) {
            vcount_target_written_ = true;
        }
        const uint8_t previous = io_[address - kIoBase];
        io_[address - kIoBase] = value;
        if (address == kDispCntAddress || address == kDispCntAddress + 1u) {
            ++dispcnt_write_count_;
            last_dispcnt_written_ = ReadIo16Direct(kDispCntAddress);
        }
        TryStartImmediateDma(address, previous);
        return;
    }
    if (address >= kPaletteBase && address < kPaletteBase + 0x01000000u) {
        const size_t offset = static_cast<size_t>((address - kPaletteBase) & (palette_.size() - 1u)) & ~size_t{1u};
        palette_[offset] = value;
        palette_[offset + 1u] = value;
        return;
    }
    if (address >= kVramBase && address < kVramBase + 0x01000000u) {
        const uint32_t offset = NormalizeVramOffset(address);
        const uint32_t mode = ReadIo16Direct(kDispCntAddress) & kModeMask;
        const uint32_t object_tile_boundary = mode >= 3u ? 0x00014000u : 0x00010000u;
        if (offset >= object_tile_boundary) {
            return; // Byte writes to OBJ tile memory are ignored.
        }
        const uint32_t aligned_address = address & ~1u;
        vram_[NormalizeVramOffset(aligned_address)] = value;
        vram_[NormalizeVramOffset(aligned_address + 1u)] = value;
        return;
    }
    if (address >= kOamBase && address < kOamBase + 0x01000000u) {
        return; // OAM accepts only halfword and word writes.
    }
    if (address >= kSaveBase && address < kSaveEnd) {
        backup_.Write8(address - kSaveBase, value);
        return;
    }
}


void MemoryBus::Write16(uint32_t address, uint16_t value) {
    address &= ~1u;
    if (address >= kPaletteBase && address < kPaletteBase + 0x01000000u) {
        const size_t offset = static_cast<size_t>((address - kPaletteBase) & (palette_.size() - 1u));
        palette_[offset] = static_cast<uint8_t>(value & 0xFFu);
        palette_[(offset + 1u) & (palette_.size() - 1u)] = static_cast<uint8_t>(value >> 8u);
        return;
    }
    if (address >= kVramBase && address < kVramBase + 0x01000000u) {
        vram_[NormalizeVramOffset(address)] = static_cast<uint8_t>(value & 0xFFu);
        vram_[NormalizeVramOffset(address + 1u)] = static_cast<uint8_t>(value >> 8u);
        return;
    }
    if (address >= kOamBase && address < kOamBase + 0x01000000u) {
        const size_t offset = static_cast<size_t>((address - kOamBase) & (oam_.size() - 1u));
        oam_[offset] = static_cast<uint8_t>(value & 0xFFu);
        oam_[(offset + 1u) & (oam_.size() - 1u)] = static_cast<uint8_t>(value >> 8u);
        return;
    }
    if (InEepromWindow(address)) {
        backup_.WriteEepromBit(value);
        return;
    }
    if (InGpio(address)) {
        WriteGpio(address, value); // the port takes 16/32-bit stores only (GBATEK)
        return;
    }
    if (address >= kSaveBase && address < kSaveEnd) {
        Write8(address, static_cast<uint8_t>(value)); // 8-bit bus: one byte reaches the chip
        return;
    }
    Write8(address, static_cast<uint8_t>(value & 0xFFu));
    Write8(address + 1u, static_cast<uint8_t>(value >> 8u));
}


void MemoryBus::Write32(uint32_t address, uint32_t value) {
    address &= ~3u;
    if (address >= kSaveBase && address < kSaveEnd) {
        Write8(address, static_cast<uint8_t>(value));
        return;
    }
    Write16(address, static_cast<uint16_t>(value & 0xFFFFu));
    Write16(address + 2u, static_cast<uint16_t>(value >> 16u));
}


} // namespace emugbds::ingba
