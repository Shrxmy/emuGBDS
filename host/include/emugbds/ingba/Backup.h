#pragma once

// GBA cartridge backup memory (GBATEK "GBA Cart Backup"): SRAM/FRAM (32 KiB),
// FLASH (64 KiB, or 128 KiB in two banks), and EEPROM (512 bytes or 8 KiB, a
// serial chip driven one bit per halfword by DMA3). The type comes from the ID
// string Nintendo's library puts in the ROM; an EEPROM's size comes from the
// save file, or from the address width of the game's first transfer. Saves go
// to a file next to the ROM.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace emugbds::ingba {

enum class BackupType { None, Sram, Flash64K, Flash128K, Eeprom };

class Backup {
public:
    // The type named by the first ID string at a word-aligned ROM offset.
    static BackupType Detect(const std::vector<uint8_t>& rom);

    // Starts a blank chip of `type`, then loads `path` if it exists.
    bool Load(BackupType type, const std::filesystem::path& path, std::string& error);
    BackupType type() const noexcept { return type_; }

    // SRAM/FLASH area, E000000h-FFFFFFFh (8-bit bus); `offset` is relative to E000000h.
    uint8_t Read8(uint32_t offset) const noexcept;
    void Write8(uint32_t offset, uint8_t value);

    // EEPROM: one bit per halfword access. DMA3 announces how many halfwords a
    // command stream has before it writes them.
    void BeginEepromStream(uint32_t bits) noexcept;
    uint16_t ReadEepromBit() noexcept;
    void WriteEepromBit(uint16_t value);

    // Writes the save file when the contents changed; returns false (with error) on failure.
    bool Flush(std::string& error);
    bool dirty() const noexcept { return dirty_; }
    // Increments on every change, so callers can wait for writes to settle before flushing.
    uint64_t revision() const noexcept { return revision_; }
    const std::vector<uint8_t>& data() const noexcept { return data_; }

private:
    enum class FlashState : uint8_t { Ready, Unlock1, Unlock2, EraseUnlock1, EraseUnlock2, EraseCommand, Program, BankSelect };

    void Changed() noexcept {
        dirty_ = true;
        ++revision_;
    }
    void WriteFlash(uint32_t offset, uint8_t value);
    void RunEepromCommand();

    BackupType type_ = BackupType::None;
    std::filesystem::path path_;
    std::vector<uint8_t> data_; // empty for an EEPROM whose size is not known yet
    FlashState flash_state_ = FlashState::Ready;
    bool flash_id_mode_ = false;
    uint32_t flash_bank_ = 0;
    std::vector<uint8_t> eeprom_stream_;
    uint32_t eeprom_expected_bits_ = 0;
    uint32_t eeprom_read_block_ = 0;
    uint32_t eeprom_read_position_ = 68; // 68 = no read in progress (the chip reports ready)
    bool dirty_ = false;
    uint64_t revision_ = 0;
};

} // namespace emugbds::ingba
