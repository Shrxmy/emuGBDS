#include "emugbds/inds/GameCard.h"

#include <algorithm>
#include <cstring>

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
    const std::vector<uint8_t>* bios7 = bios7_;
    Backup backup = std::move(backup_);
    backup.Deselect();
    *this = GameCard{};
    rom_ = rom;
    bios7_ = bios7;
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
    } else if (mode_ == Mode::Key1) {
        // The register sends byte 0 first; as a KEY1 block, byte 0 is the most significant one.
        uint64_t value = 0u;
        for (uint32_t i = 0u; i < 8u; ++i) {
            value = (value << 8u) | ((command_[i / 4u] >> ((i % 4u) * 8u)) & 0xFFu);
        }
        StartKey1Command(value);
    } else if (mode_ == Mode::Raw && command == 0x3Cu) {
        ActivateKey1();
        source_ = Source::HighZ;
    } else if (mode_ == Mode::Main && command == 0xB7u) {
        source_ = Source::Rom;
        mirror_size_ = 0x8000u;
        while (mirror_size_ < rom_->size() && mirror_size_ < 0x80000000u) {
            mirror_size_ <<= 1u;
        }
    } else if ((mode_ == Mode::Raw && command == 0x90u) || (mode_ == Mode::Main && command == 0xB8u)) {
        source_ = Source::ChipId;
    } else if (mode_ == Mode::Raw && command == 0x00u) {
        source_ = Source::Header;
    } else if (mode_ == Mode::Raw && command == 0x9Fu) {
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


void GameCard::ActivateKey1() noexcept {
    mode_ = Mode::Key1;
    last_secure_command_ = 0u;
    secure_offset_ = 0u;
    const uint32_t gamecode = RomByte(0x0Cu) | (RomByte(0x0Du) << 8u) | (RomByte(0x0Eu) << 16u) |
        (static_cast<uint32_t>(RomByte(0x0Fu)) << 24u);
    Key1 secure_key{};
    key1_ready_ = bios7_ != nullptr && key1_.Init(*bios7_, gamecode, 2, 8u) && secure_key.Init(*bios7_, gamecode, 3, 8u);

    secure_area_.resize(0x4000u);
    for (uint32_t i = 0u; i < 0x4000u; ++i) {
        secure_area_[i] = RomByte(0x4000u + i);
    }
    // A decrypted ROM image holds the ID "encryObj", or E7FFDEFFh twice once the BIOS has checked it;
    // a cartridge stores the first 2 KiB encrypted, which the BIOS undoes as level 2 on the first
    // 8 bytes and then level 3 on all 2 KiB. Other images are taken as already encrypted.
    static constexpr uint8_t kId[8] = {'e', 'n', 'c', 'r', 'y', 'O', 'b', 'j'};
    static constexpr uint8_t kCheckedId[8] = {0xFFu, 0xDEu, 0xFFu, 0xE7u, 0xFFu, 0xDEu, 0xFFu, 0xE7u};
    const bool decrypted = std::equal(kId, kId + 8, secure_area_.begin()) ||
        std::equal(kCheckedId, kCheckedId + 8, secure_area_.begin());
    if (!key1_ready_ || !decrypted) {
        return;
    }
    std::copy(kId, kId + 8, secure_area_.begin());
    const auto block = [this](uint32_t offset, const Key1& key) {
        uint32_t words[2] = {};
        std::memcpy(words, &secure_area_[offset], 8u);
        key.Encrypt(words[0], words[1]);
        std::memcpy(&secure_area_[offset], words, 8u);
    };
    for (uint32_t offset = 0u; offset < 0x800u; offset += 8u) {
        block(offset, secure_key);
    }
    block(0u, key1_);
}


void GameCard::StartKey1Command(uint64_t command) noexcept {
    source_ = Source::Zero; // commands 4 and 6, dummy and invalid replies
    if (!key1_ready_) {
        return;
    }
    uint32_t lo = static_cast<uint32_t>(command);
    uint32_t hi = static_cast<uint32_t>(command >> 32u);
    key1_.Decrypt(lo, hi);
    const uint64_t plain = (static_cast<uint64_t>(hi) << 32u) | lo;
    switch (plain >> 60u) {
    case 0x1u: // 1lllliiijjjkkkkk: 2nd chip ID
        source_ = Source::ChipId;
        break;
    case 0x2u: // 2bbbbiiijjjkkkkk: secure area block bbbb (4-7)
        source_ = Source::Secure;
        address_ = static_cast<uint32_t>((plain >> 44u) & 0xFFFFu) * 0x1000u;
        if (command != last_secure_command_) {
            secure_offset_ = 0u;
            last_secure_command_ = command;
        }
        break;
    case 0xAu: // Alllliiijjjkkkkk: enter main data mode
        mode_ = Mode::Main;
        break;
    default:
        break;
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
        case Source::Secure: {
            const uint32_t address = address_ + (secure_offset_++ & 0xFFFu);
            byte = address >= 0x4000u && address < 0x8000u ? secure_area_[address - 0x4000u] : 0u;
            break;
        }
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
