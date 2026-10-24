#include "emugbds/ingba/MemoryBus.h"
#include "emugbds/ingba/MemoryConstants.h"
#include "emugbds/ingba/VideoConstants.h"

#include <algorithm>

namespace emugbds::ingba {

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

    rom_ = std::move(rom_image);
    std::fill(flash_.begin(), flash_.end(), 0xFFu);
    flash_bank_ = 0u;
    ResetFlashCommandState();
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
        if (timer + 1u < timer_counters_.size() &&
            (timer_control_[timer + 1u] & 0x0084u) == 0x0084u) {
            IncrementTimer(timer + 1u, 1u);
        }
    }
}

void MemoryBus::AdvanceTimers(uint32_t cycles) noexcept {
    if (cycles == 0u) {
        return;
    }

    constexpr std::array<uint32_t, 4u> kPrescalers = {1u, 64u, 256u, 1024u};
    for (uint32_t timer = 0u; timer < timer_counters_.size(); ++timer) {
        const uint16_t control = timer_control_[timer];
        if ((control & 0x0080u) == 0u || (control & 0x0004u) != 0u) {
            continue;
        }

        const uint32_t prescaler = kPrescalers[control & 0x3u];
        const uint64_t accumulated = static_cast<uint64_t>(timer_prescaler_phase_[timer]) + cycles;
        const uint32_t increments = static_cast<uint32_t>(accumulated / prescaler);
        timer_prescaler_phase_[timer] = static_cast<uint32_t>(accumulated % prescaler);
        IncrementTimer(timer, increments);
    }
}

uint8_t MemoryBus::ReadFlash(uint32_t address) const noexcept {
    const uint32_t offset = (address - kSaveBase) & (kSaveWindowSize - 1u);
    if (flash_id_mode_) {
        if (offset == 0u) {
            return 0xC2u; // Macronix-compatible manufacturer ID used by Emerald.
        }
        if (offset == 1u) {
            return 0x09u; // 128 KiB flash device ID.
        }
        return 0xFFu;
    }

    const size_t bank_offset = static_cast<size_t>(flash_bank_ & 1u) * kSaveWindowSize;
    return flash_[bank_offset + offset];
}

void MemoryBus::ResetFlashCommandState() noexcept {
    flash_command_state_ = FlashCommandState::Ready;
    flash_id_mode_ = false;
}

void MemoryBus::WriteFlash(uint32_t address, uint8_t value) noexcept {
    const uint32_t offset = (address - kSaveBase) & (kSaveWindowSize - 1u);
    if (value == 0xF0u && flash_command_state_ != FlashCommandState::Program) {
        ResetFlashCommandState();
        return;
    }

    switch (flash_command_state_) {
    case FlashCommandState::Ready:
        if (offset == 0x5555u && value == 0xAAu) {
            flash_command_state_ = FlashCommandState::Unlock1;
        }
        return;
    case FlashCommandState::Unlock1:
        if (offset == 0x2AAAu && value == 0x55u) {
            flash_command_state_ = FlashCommandState::Unlock2;
        } else {
            ResetFlashCommandState();
        }
        return;
    case FlashCommandState::Unlock2:
        if (offset != 0x5555u) {
            ResetFlashCommandState();
            return;
        }
        switch (value) {
        case 0x90u:
            flash_id_mode_ = true;
            flash_command_state_ = FlashCommandState::Ready;
            break;
        case 0xA0u:
            flash_command_state_ = FlashCommandState::Program;
            break;
        case 0x80u:
            flash_command_state_ = FlashCommandState::EraseUnlock1;
            break;
        case 0xB0u:
            flash_command_state_ = FlashCommandState::BankSelect;
            break;
        default:
            ResetFlashCommandState();
            break;
        }
        return;
    case FlashCommandState::EraseUnlock1:
        if (offset == 0x5555u && value == 0xAAu) {
            flash_command_state_ = FlashCommandState::EraseUnlock2;
        } else {
            ResetFlashCommandState();
        }
        return;
    case FlashCommandState::EraseUnlock2:
        if (offset == 0x2AAAu && value == 0x55u) {
            flash_command_state_ = FlashCommandState::EraseCommand;
        } else {
            ResetFlashCommandState();
        }
        return;
    case FlashCommandState::EraseCommand:
        if (offset == 0x5555u && value == 0x10u) {
            std::fill(flash_.begin(), flash_.end(), 0xFFu);
        } else if (value == 0x30u) {
            const size_t bank_offset = static_cast<size_t>(flash_bank_ & 1u) * kSaveWindowSize;
            const size_t sector_offset = static_cast<size_t>(offset) & ~(kFlashSectorSize - 1u);
            std::fill_n(flash_.begin() + bank_offset + sector_offset, kFlashSectorSize, 0xFFu);
        }
        ResetFlashCommandState();
        return;
    case FlashCommandState::Program: {
        const size_t bank_offset = static_cast<size_t>(flash_bank_ & 1u) * kSaveWindowSize;
        flash_[bank_offset + offset] &= value;
        ResetFlashCommandState();
        return;
    }
    case FlashCommandState::BankSelect:
        if (offset == 0u) {
            flash_bank_ = static_cast<uint8_t>(value & 1u);
        }
        ResetFlashCommandState();
        return;
    }
}

void MemoryBus::TryStartImmediateDma(uint32_t control_high_address) {
    for (uint32_t channel = 0; channel < kDmaControlHighByteAddresses.size(); ++channel) {
        if (kDmaControlHighByteAddresses[channel] != control_high_address) {
            continue;
        }
        const uint32_t control_address = kDmaBaseAddresses[channel] + 10u;
        const uint16_t control = ReadIo16Direct(control_address);
        if ((control & kDmaEnableBit) == 0u) {
            return;
        }
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
    const uint32_t initial_source = ReadIo32Direct(base + 0u);
    const uint32_t initial_destination = ReadIo32Direct(base + 4u);
    uint32_t source = initial_source;
    uint32_t destination = initial_destination;

    uint16_t count = ReadIo16Direct(base + 8u);
    uint16_t control = ReadIo16Direct(base + 10u);

    const bool word_transfer = (control & kDmaTransfer32Bit) != 0u;
    const uint32_t unit_size = word_transfer ? 4u : 2u;
    uint32_t units = count;
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

    const int32_t source_delta = compute_delta(source_control, true);
    const int32_t destination_delta = compute_delta(destination_control, false);

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

    WriteIo32Direct(base + 0u, source);
    if (destination_reload && (control & kDmaRepeatBit) != 0u) {
        WriteIo32Direct(base + 4u, initial_destination);
    } else {
        WriteIo32Direct(base + 4u, destination);
    }
    WriteIo16Direct(base + 8u, count);
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
    if (address >= kSaveBase && address < kSaveBase + kSaveWindowSize) {
        return ReadFlash(address);
    }
    if (address >= kRomBase && address < kSaveBase && !rom_.empty()) {
        const uint32_t rom_offset = (address - kRomBase) % static_cast<uint32_t>(rom_.size());
        return rom_[rom_offset];
    }
    return 0;
}


uint16_t MemoryBus::Read16(uint32_t address) const {
    const uint16_t lo = Read8(address);
    const uint16_t hi = Read8(address + 1);
    return static_cast<uint16_t>(lo | (hi << 8));
}


uint32_t MemoryBus::Read32(uint32_t address) const {
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

        io_[address - kIoBase] = value;
        if (address == kDispCntAddress || address == kDispCntAddress + 1u) {
            ++dispcnt_write_count_;
            last_dispcnt_written_ = ReadIo16Direct(kDispCntAddress);
        }
        TryStartImmediateDma(address);
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
    if (address >= kSaveBase && address < kSaveBase + kSaveWindowSize) {
        WriteFlash(address, value);
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
    Write8(address, static_cast<uint8_t>(value & 0xFFu));
    Write8(address + 1u, static_cast<uint8_t>(value >> 8u));
}


void MemoryBus::Write32(uint32_t address, uint32_t value) {
    address &= ~3u;
    Write16(address, static_cast<uint16_t>(value & 0xFFFFu));
    Write16(address + 2u, static_cast<uint16_t>(value >> 16u));
}


} // namespace emugbds::ingba
