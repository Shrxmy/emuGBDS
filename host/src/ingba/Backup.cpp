#include "emugbds/ingba/Backup.h"

#include "emugbds/core/SaveFile.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>

namespace emugbds::ingba {
namespace {

constexpr uint32_t kSramSize = 0x8000u;
constexpr uint32_t kFlashBankSize = 0x10000u;
constexpr uint32_t kFlashSectorSize = 0x1000u;
constexpr uint32_t kEepromSmall = 0x200u;  // 6-bit block addresses
constexpr uint32_t kEepromLarge = 0x2000u; // 14-bit block addresses (10 used)
constexpr uint32_t kEepromReadBits = 68u;  // 4 dummy bits, then 64 data bits

uint32_t ChipSize(BackupType type) noexcept {
    switch (type) {
    case BackupType::Sram: return kSramSize;
    case BackupType::Flash64K: return kFlashBankSize;
    case BackupType::Flash128K: return 2u * kFlashBankSize;
    default: return 0u;
    }
}

} // namespace

BackupType Backup::Detect(const std::vector<uint8_t>& rom) {
    // GBATEK "GBA Cart Backup IDs"; "SRAM_" also matches FRAM carts ("SRAM_F_V").
    struct Id {
        const char* text;
        BackupType type;
    };
    static constexpr std::array<Id, 5> kIds = {{
        {"EEPROM_V", BackupType::Eeprom},
        {"SRAM_", BackupType::Sram},
        {"FLASH_V", BackupType::Flash64K},
        {"FLASH512_V", BackupType::Flash64K},
        {"FLASH1M_V", BackupType::Flash128K},
    }};
    for (size_t offset = 0u; offset + 12u <= rom.size(); offset += 4u) {
        for (const Id& id : kIds) {
            const size_t length = std::strlen(id.text);
            if (std::memcmp(rom.data() + offset, id.text, length) == 0) {
                return id.type;
            }
        }
    }
    return BackupType::None;
}


bool Backup::Load(BackupType type, const std::filesystem::path& path, std::string& error) {
    *this = Backup{};
    type_ = type;
    path_ = path;
    data_.assign(ChipSize(type), 0xFFu); // erased FLASH reads FFh; SRAM gets the same blank pattern
    std::error_code exists_error;
    const bool exists = type != BackupType::None && std::filesystem::exists(path, exists_error);
    if (exists_error) {
        error = "Cannot check save file " + path.string() + ": " + exists_error.message(); // never start blank over it
        return false;
    }
    if (!exists) {
        return true;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        error = "Failed to open save file: " + path.string();
        return false;
    }
    const std::vector<uint8_t> file(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>{});
    if (input.bad()) {
        error = "Failed to read save file: " + path.string();
        return false;
    }
    if (type == BackupType::Eeprom) {
        if (file.size() != kEepromSmall && file.size() != kEepromLarge) {
            error = "EEPROM save file must be 512 bytes or 8 KiB: " + path.string();
            return false;
        }
        data_ = file;
        return true;
    }
    // A smaller file (e.g. a 64K FLASH save for a 128K chip) fills the start; a larger one would lose data.
    if (file.size() > data_.size()) {
        error = "Save file is larger than this game's save chip: " + path.string();
        return false;
    }
    std::copy(file.begin(), file.end(), data_.begin());
    return true;
}


uint8_t Backup::Read8(uint32_t offset) const noexcept {
    switch (type_) {
    case BackupType::Sram:
        return data_[offset & (kSramSize - 1u)];
    case BackupType::Flash64K:
    case BackupType::Flash128K: {
        offset &= kFlashBankSize - 1u;
        if (flash_id_mode_ && offset < 2u) {
            // Macronix IDs (GBATEK "Device Types"): 1CC2h for 64K, 09C2h for 128K.
            return offset == 0u ? 0xC2u : type_ == BackupType::Flash64K ? 0x1Cu : 0x09u;
        }
        return data_[flash_bank_ * kFlashBankSize + offset];
    }
    default:
        return 0xFFu; // no chip (an EEPROM is not mapped here)
    }
}


void Backup::Write8(uint32_t offset, uint8_t value) {
    if (type_ == BackupType::Sram) {
        uint8_t& byte = data_[offset & (kSramSize - 1u)];
        if (byte != value) {
            byte = value;
            Changed();
        }
    } else if (type_ == BackupType::Flash64K || type_ == BackupType::Flash128K) {
        WriteFlash(offset & (kFlashBankSize - 1u), value);
    }
}


// GBATEK "GBA Cart Backup Flash ROM": commands are AAh to 5555h, 55h to 2AAAh, then the command to 5555h.
void Backup::WriteFlash(uint32_t offset, uint8_t value) {
    const auto reset = [this] {
        flash_state_ = FlashState::Ready;
        flash_id_mode_ = false;
    };
    if (value == 0xF0u && flash_state_ != FlashState::Program) {
        reset(); // terminate ID mode, or abort a command
        return;
    }
    switch (flash_state_) {
    case FlashState::Ready:
        if (offset == 0x5555u && value == 0xAAu) {
            flash_state_ = FlashState::Unlock1;
        }
        return;
    case FlashState::Unlock1:
        if (offset == 0x2AAAu && value == 0x55u) {
            flash_state_ = FlashState::Unlock2;
        } else {
            reset();
        }
        return;
    case FlashState::Unlock2:
        if (offset != 0x5555u) {
            reset();
            return;
        }
        switch (value) {
        case 0x90u:
            flash_id_mode_ = true;
            flash_state_ = FlashState::Ready;
            break;
        case 0xA0u:
            flash_state_ = FlashState::Program;
            break;
        case 0x80u:
            flash_state_ = FlashState::EraseUnlock1;
            break;
        case 0xB0u:
            flash_state_ = FlashState::BankSelect;
            break;
        default:
            reset();
            break;
        }
        return;
    case FlashState::EraseUnlock1:
        if (offset == 0x5555u && value == 0xAAu) {
            flash_state_ = FlashState::EraseUnlock2;
        } else {
            reset();
        }
        return;
    case FlashState::EraseUnlock2:
        if (offset == 0x2AAAu && value == 0x55u) {
            flash_state_ = FlashState::EraseCommand;
        } else {
            reset();
        }
        return;
    case FlashState::EraseCommand:
        if (offset == 0x5555u && value == 0x10u) {
            std::fill(data_.begin(), data_.end(), 0xFFu);
            Changed();
        } else if (value == 0x30u) {
            const size_t sector = flash_bank_ * kFlashBankSize + (offset & ~(kFlashSectorSize - 1u));
            std::fill_n(data_.begin() + static_cast<std::ptrdiff_t>(sector), kFlashSectorSize, 0xFFu);
            Changed();
        }
        reset();
        return;
    case FlashState::Program: {
        // Programming can only clear bits; GBATEK requires an erased (FFh) target.
        uint8_t& byte = data_[flash_bank_ * kFlashBankSize + offset];
        byte &= value;
        Changed();
        reset();
        return;
    }
    case FlashState::BankSelect:
        if (offset == 0u && type_ == BackupType::Flash128K) {
            flash_bank_ = value & 1u; // 64K chips have a single bank
        }
        reset();
        return;
    }
}


void Backup::BeginEepromStream(uint32_t bits) noexcept {
    eeprom_stream_.clear();
    eeprom_expected_bits_ = bits;
}


// GBATEK "GBA Cart Backup EEPROM": after a read request, 4 ignored bits then 64 data bits, MSB
// first; otherwise the chip reports ready (bit 0 set), as it finishes writes at once.
uint16_t Backup::ReadEepromBit() noexcept {
    if (eeprom_read_position_ >= kEepromReadBits) {
        return 1u;
    }
    const uint32_t position = eeprom_read_position_++;
    if (position < 4u || data_.empty()) {
        return 0u;
    }
    const uint32_t bit = position - 4u;
    return (data_[eeprom_read_block_ * 8u + bit / 8u] >> (7u - bit % 8u)) & 1u;
}


void Backup::WriteEepromBit(uint16_t value) {
    if (type_ != BackupType::Eeprom || eeprom_expected_bits_ == 0u) {
        return; // only DMA3 streams reach the chip (GBATEK: STRH transfers do not work)
    }
    eeprom_stream_.push_back(static_cast<uint8_t>(value & 1u));
    if (eeprom_stream_.size() == eeprom_expected_bits_) {
        RunEepromCommand();
        BeginEepromStream(0u);
    }
}


// A finished stream: "11" + address + "0" requests a read, "10" + address + 64 data bits + "0" writes.
void Backup::RunEepromCommand() {
    const size_t bits = eeprom_stream_.size();
    if (bits < 3u || eeprom_stream_[0] != 1u) {
        return;
    }
    const bool read = eeprom_stream_[1] != 0u;
    const size_t address_bits = read ? bits - 3u : bits >= 67u ? bits - 67u : 0u;
    if (address_bits != 6u && address_bits != 14u) {
        return;
    }
    // The game's address width tells the chip size. A 512-byte save used with 14-bit addresses
    // grows to 8K (its 64 blocks keep their place); an 8K save used with 6-bit addresses is kept.
    if (data_.size() < (address_bits == 6u ? kEepromSmall : kEepromLarge)) {
        data_.resize(address_bits == 6u ? kEepromSmall : kEepromLarge, 0xFFu);
    }
    uint32_t block = 0u;
    for (size_t i = 0u; i < address_bits; ++i) {
        block = (block << 1u) | eeprom_stream_[2u + i];
    }
    block &= address_bits == 6u ? 0x3Fu : 0x3FFu; // 8K chips use the low 10 bits
    if (read) {
        eeprom_read_block_ = block;
        eeprom_read_position_ = 0u;
        return;
    }
    for (uint32_t i = 0u; i < 8u; ++i) {
        uint8_t byte = 0u;
        for (uint32_t b = 0u; b < 8u; ++b) {
            byte = static_cast<uint8_t>((byte << 1u) | eeprom_stream_[2u + address_bits + i * 8u + b]);
        }
        data_[block * 8u + i] = byte;
    }
    Changed();
}


bool Backup::Flush(std::string& error) {
    if (!dirty_ || type_ == BackupType::None || data_.empty() || path_.empty()) {
        return true;
    }
    if (!core::WriteFileReplacing(path_, data_, error)) {
        return false;
    }
    dirty_ = false;
    return true;
}

} // namespace emugbds::ingba
