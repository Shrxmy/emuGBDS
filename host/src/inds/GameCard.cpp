#include "emugbds/inds/GameCard.h"

namespace emugbds::inds {
namespace {

constexpr uint32_t kAuxSpi = 0x00u;    // AUXSPICNT | AUXSPIDATA << 16
constexpr uint32_t kRomCtrl = 0x04u;
constexpr uint32_t kCommandLo = 0x08u;
constexpr uint32_t kCommandHi = 0x0Cu;
constexpr uint32_t kSeedsFirst = 0x10u;
constexpr uint32_t kSeedsLast = 0x18u;
constexpr uint16_t kAuxSpiWritable = 0xE043u; // baudrate, hold, slot mode, IRQ enable, slot enable
constexpr uint16_t kAuxSpiTransferIrq = 1u << 14;
constexpr uint16_t kAuxSpiHold = 1u << 6;
constexpr uint16_t kAuxSpiSerialMode = 1u << 13; // the slot talks to the backup chip
constexpr uint16_t kAuxSpiEnable = 1u << 15;
constexpr uint32_t kRomCtrlRelease = 1u << 29;
constexpr uint32_t kEmptySlotData = 0xFFFFFFFFu;

} // namespace

void GameCard::Reset() noexcept {
    const std::vector<uint8_t>* rom = rom_;
    Backup backup = std::move(backup_);
    backup.Deselect();
    *this = GameCard{};
    rom_ = rom;
    backup_ = std::move(backup);
}


uint32_t GameCard::ChipId() const noexcept {
    if (rom_ == nullptr || rom_->empty()) {
        return kEmptySlotData;
    }
    uint32_t megabytes = 1u;
    while (megabytes < 0x1000u && (static_cast<uint64_t>(megabytes) << 20u) < rom_->size()) {
        megabytes <<= 1u;
    }
    // Size byte: (N+1) MiB up to 128 MiB, then F0h-FFh = (100h-N) x 256 MiB.
    const uint32_t size_code = megabytes <= 0x80u ? megabytes - 1u : 0x100u - megabytes / 256u;
    // Byte 3 bit 7 marks the newer protocol used by carts of 128 MiB and up.
    return 0xC2u | (size_code << 8u) | (megabytes >= 0x80u ? 0x80000000u : 0u);
}


uint32_t GameCard::ReadWord(uint32_t offset) const noexcept {
    switch (offset) {
    case kAuxSpi:
        return auxspicnt_ | (static_cast<uint32_t>(auxspidata_) << 16u); // SPI is never busy here
    case kRomCtrl:
        return romctrl_;
    case kCommandLo:
        return command_[0];
    case kCommandHi:
        return command_[1];
    default:
        return 0u; // KEY2 seeds are write-only
    }
}


void GameCard::WriteWord(uint32_t offset, uint32_t value, uint32_t mask) noexcept {
    const auto merge = [&](uint32_t old_value) { return (old_value & ~mask) | (value & mask); };
    switch (offset) {
    case kAuxSpi:
        auxspicnt_ = static_cast<uint16_t>((auxspicnt_ & ~(mask & kAuxSpiWritable)) | (value & mask & kAuxSpiWritable));
        if ((auxspicnt_ & (kAuxSpiSerialMode | kAuxSpiEnable)) != (kAuxSpiSerialMode | kAuxSpiEnable)) {
            backup_.Deselect(); // Leaving SPI mode releases the backup chip.
        }
        if ((mask & 0x00FF0000u) != 0u) {
            // Writing AUXSPIDATA starts an exchange that completes at once; no card answers FFh.
            const bool spi = (auxspicnt_ & (kAuxSpiSerialMode | kAuxSpiEnable)) == (kAuxSpiSerialMode | kAuxSpiEnable);
            auxspidata_ = spi && rom_ != nullptr && !rom_->empty() ? backup_.Transfer(static_cast<uint8_t>(value >> 16u)) : 0xFFu;
            if ((auxspicnt_ & kAuxSpiHold) == 0u) {
                backup_.Deselect();
            }
        }
        return;
    case kRomCtrl: {
        // Bit 23 is read-only, bit 15 write-only, bit 29 sticks once set.
        const uint32_t writable = mask & ~(kDataReady | (1u << 15));
        const uint32_t updated = ((romctrl_ & ~writable) | (value & writable)) | (romctrl_ & kRomCtrlRelease);
        const bool start = (mask & value & kBusy) != 0u && (romctrl_ & kBusy) == 0u;
        romctrl_ = (updated & ~kBusy) | (romctrl_ & kBusy);
        if (start) {
            StartTransfer();
        }
        return;
    }
    case kCommandLo:
        command_[0] = merge(command_[0]);
        return;
    case kCommandHi:
        command_[1] = merge(command_[1]);
        return;
    default:
        if (offset >= kSeedsFirst && offset <= kSeedsLast) {
            uint32_t& seed = seeds_[(offset - kSeedsFirst) / 4u];
            seed = merge(seed);
        }
        return;
    }
}


void GameCard::StartTransfer() noexcept {
    const uint32_t block = (romctrl_ >> 24u) & 7u;
    words_remaining_ = block == 0u ? 0u : block == 7u ? 1u : (0x100u << block) / 4u;
    romctrl_ |= kBusy;
    // Command bytes go out MSB first from 40001A8h: byte 0 is the opcode, bytes 1-4 the address.
    const uint32_t command = command_[0] & 0xFFu;
    address_ = ((command_[0] >> 8u) & 0xFFu) << 24u | ((command_[0] >> 16u) & 0xFFu) << 16u |
        ((command_[0] >> 24u) & 0xFFu) << 8u | (command_[1] & 0xFFu);
    offset_ = 0u;
    if (rom_ == nullptr || rom_->empty()) {
        source_ = Source::HighZ;
    } else if (command == 0xB7u) {
        source_ = Source::Rom;
        mirror_size_ = 0x8000u;
        while (mirror_size_ < rom_->size() && mirror_size_ < 0x80000000u) {
            mirror_size_ <<= 1u;
        }
    } else if (command == 0x90u || command == 0xB8u) {
        source_ = Source::ChipId;
    } else if (command == 0x00u) {
        source_ = Source::Header;
    } else if (command == 0x9Fu) {
        source_ = Source::HighZ;
    } else {
        source_ = Source::Zero; // Invalid commands in data mode return 00h bytes.
    }
    if (words_remaining_ == 0u) {
        FinishTransfer();
    } else {
        romctrl_ |= kDataReady;
    }
}


uint32_t GameCard::ReadData() noexcept {
    if (words_remaining_ == 0u) {
        return kEmptySlotData;
    }
    const uint32_t word = NextWord();
    if (--words_remaining_ == 0u) {
        FinishTransfer();
    }
    return word;
}


uint32_t GameCard::NextWord() noexcept {
    uint32_t word = 0u;
    for (uint32_t i = 0u; i < 4u; ++i, ++offset_) {
        uint8_t byte = 0u;
        switch (source_) {
        case Source::HighZ: byte = 0xFFu; break;
        case Source::Zero: byte = 0u; break;
        case Source::ChipId: byte = static_cast<uint8_t>(ChipId() >> ((offset_ & 3u) * 8u)); break;
        case Source::Header: byte = RomByte(offset_ & 0xFFFu); break; // The header repeats every 1000h bytes.
        case Source::Rom: {
            // The stream wraps within the current 4 KiB block; 0-7FFFh reads as 8000h + (addr & 1FFh).
            uint32_t address = (address_ & ~0xFFFu) | ((address_ + offset_) & 0xFFFu);
            address &= mirror_size_ - 1u; // Addresses past the ROM mirror
            if (address < 0x8000u) {
                address = 0x8000u + (address & 0x1FFu);
            }
            byte = RomByte(address);
            break;
        }
        }
        word |= static_cast<uint32_t>(byte) << (i * 8u);
    }
    return word;
}


uint8_t GameCard::RomByte(uint32_t address) const noexcept {
    return address < rom_->size() ? (*rom_)[address] : 0xFFu; // Unused space past the image reads FFh
}


void GameCard::FinishTransfer() noexcept {
    romctrl_ &= ~(kBusy | kDataReady);
    if ((auxspicnt_ & kAuxSpiTransferIrq) != 0u) {
        transfer_irq_ = true;
    }
}

} // namespace emugbds::inds
