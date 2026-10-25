#pragma once

// NDS cartridge backup memory on the AUXSPI bus: EEPROM (0.5K with 1-byte
// addresses, 8K/64K with 2-byte), and FLASH (3-byte addresses), GBATEK "DS
// Cartridge Backup". The chip type is not in the ROM, so it comes from the size
// of an existing save file, or from the first write session (FLASH-only commands,
// or the address width that leaves a whole page of data). Until then the chip is
// blank, which reads FFh at any address width. Memory grows to cover the highest
// address used. Saves go to a file next to the ROM.
// shortcut: write-protect bits are stored but not enforced.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace emugbds::inds {

class Backup {
public:
    // Loads `path` if it exists (its size selects the chip type); otherwise starts blank.
    bool Load(const std::filesystem::path& path, std::string& error);
    void Clear() noexcept { *this = Backup{}; }
    // One SPI byte exchange; the session ends at Deselect (chip select released).
    uint8_t Transfer(uint8_t out);
    void Deselect();
    // Writes the save file when the contents changed; returns false (with error) on failure.
    bool Flush(std::string& error);
    bool dirty() const noexcept { return dirty_; }
    // Increments on every change, so callers can wait for writes to settle before flushing.
    uint64_t revision() const noexcept { return revision_; }
    uint32_t address_bytes() const noexcept { return address_bytes_; }
    const std::vector<uint8_t>& data() const noexcept { return data_; }

private:
    uint8_t Process(uint32_t position, uint8_t out);
    uint8_t* At(uint32_t address, bool grow);
    uint32_t PageSize() const noexcept;

    std::filesystem::path path_;
    std::vector<uint8_t> data_;
    uint32_t address_bytes_ = 0; // 0 = type not known yet
    bool flash_ = false;
    bool write_enabled_ = false;
    bool session_write_enabled_ = false; // latch state when the current command began
    uint8_t status_ = 0;         // write-protect bits written by WRSR
    std::vector<uint8_t> session_;
    uint32_t address_ = 0;
    bool dirty_ = false;
    uint64_t revision_ = 0;
};

} // namespace emugbds::inds
