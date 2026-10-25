#include "emugbds/inds/Backup.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>

namespace emugbds::inds {
namespace {

constexpr uint8_t kWriteEnable = 0x06u;
constexpr uint8_t kWriteDisable = 0x04u;
constexpr uint8_t kReadStatus = 0x05u;
constexpr uint8_t kWriteStatus = 0x01u;
constexpr uint8_t kReadId = 0x9Fu;
constexpr uint8_t kRead = 0x03u;      // 0.5K EEPROM: RDLO
constexpr uint8_t kReadHigh = 0x0Bu;  // 0.5K EEPROM: RDHI; FLASH: fast read (one dummy byte)
constexpr uint8_t kWrite = 0x02u;     // 0.5K EEPROM: WRLO; FLASH: page program
constexpr uint8_t kWriteHigh = 0x0Au; // 0.5K EEPROM: WRHI; FLASH: page write
constexpr uint8_t kPageErase = 0xDBu; // FLASH only
constexpr uint8_t kSectorErase = 0xD8u;
constexpr uint32_t kMaxSize = 0x800000u;

bool FlashOnly(uint8_t command) noexcept {
    return command == kReadId || command == kPageErase || command == kSectorErase;
}

} // namespace

bool Backup::Load(const std::filesystem::path& path, std::string& error) {
    Clear();
    path_ = path;
    std::error_code exists_error;
    if (!std::filesystem::exists(path, exists_error)) {
        return true; // New save: blank chip, type found on first write.
    }
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        error = "Failed to open save file: " + path.string();
        return false;
    }
    data_.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    const size_t size = data_.size();
    if (size == 0u || size > kMaxSize || (size & (size - 1u)) != 0u) {
        error = "Save file size is not a power of two up to 8 MiB: " + path.string();
        data_.clear();
        return false;
    }
    address_bytes_ = size <= 0x200u ? 1u : size <= 0x10000u ? 2u : 3u;
    flash_ = size >= 0x40000u; // 128K is a 3-byte-address EEPROM; 256K and up are FLASH
    return true;
}


uint8_t Backup::Transfer(uint8_t out) {
    session_.push_back(out);
    const uint32_t position = static_cast<uint32_t>(session_.size() - 1u);
    if (address_bytes_ == 0u) {
        const uint8_t command = session_[0];
        if (FlashOnly(command)) {
            address_bytes_ = 3u;
            flash_ = true;
        } else if (command == kWrite || command == kWriteHigh) {
            return 0xFFu; // Decided when the write session ends.
        } else if (command == kRead || command == kReadHigh) {
            return 0xFFu; // Blank chip: FFh at any address width.
        }
    }
    return Process(position, out);
}


void Backup::Deselect() {
    if (address_bytes_ == 0u && !session_.empty() && (session_[0] == kWrite || session_[0] == kWriteHigh)) {
        // Pick the address width that leaves exactly one page of data (16, 32/128, or 256 bytes).
        const size_t length = session_.size();
        if (length == 1u + 1u + 16u) {
            address_bytes_ = 1u;
        } else if (length == 1u + 3u + 256u) {
            address_bytes_ = 3u;
            flash_ = true;
        } else if (length == 1u + 2u + 32u) {
            address_bytes_ = 2u;
        } else if (length == 1u + 2u + 128u) {
            address_bytes_ = 2u;
            data_.resize(0x10000u, 0xFFu); // 128-byte pages: a 64K EEPROM
        } else {
            // shortcut: a partial first page is ambiguous between address widths, so this guess can be
            // wrong (e.g. a one-byte write to a 0.5K EEPROM); an existing save file avoids the guess.
            address_bytes_ = length > 1u + 3u + 128u ? 3u : 2u;
            flash_ = address_bytes_ == 3u;
        }
        const std::vector<uint8_t> replay = session_;
        session_.clear();
        for (uint8_t byte : replay) {
            session_.push_back(byte);
            Process(static_cast<uint32_t>(session_.size() - 1u), byte);
        }
    }
    if (!session_.empty() && (session_[0] == kWrite || session_[0] == kWriteHigh || session_[0] == kPageErase ||
                                 session_[0] == kSectorErase)) {
        write_enabled_ = false; // A finished write or erase resets the write-enable latch.
    }
    session_.clear();
}


uint8_t Backup::Process(uint32_t position, uint8_t out) {
    const uint8_t command = session_[0];
    if (position == 0u) {
        address_ = 0u;
        session_write_enabled_ = write_enabled_; // writes and erases need WREN beforehand
        if (command == kWriteEnable) {
            write_enabled_ = true;
        } else if (command == kWriteDisable) {
            write_enabled_ = false;
        }
        return 0xFFu;
    }
    switch (command) {
    case kReadStatus: {
        const uint8_t fixed = address_bytes_ == 1u ? 0xF0u : 0x00u; // 0.5K EEPROM: bits 4-7 read as ones
        return static_cast<uint8_t>(fixed | status_ | (write_enabled_ ? 0x02u : 0x00u)); // never busy
    }
    case kWriteStatus:
        if (position == 1u && session_write_enabled_) {
            status_ = static_cast<uint8_t>(out & 0x0Cu); // write-protect bits
            write_enabled_ = false;
        }
        return 0xFFu;
    case kReadId: {
        // FLASH JEDEC ID (ST-style), capacity byte from the size; EEPROMs answer FFh.
        uint8_t capacity = 0x12u;
        for (size_t size = data_.size(); size > 0x40000u; size >>= 1u) {
            ++capacity;
        }
        const uint8_t id[3] = {0x20u, 0x40u, capacity};
        return flash_ && position <= 3u ? id[position - 1u] : 0xFFu;
    }
    case kRead:
    case kReadHigh:
    case kWrite:
    case kWriteHigh:
    case kPageErase:
    case kSectorErase: {
        const bool eeprom512 = address_bytes_ == 1u;
        if (position <= address_bytes_) {
            address_ = (address_ << 8u) | out; // MSB first
            if (eeprom512 && (command == kReadHigh || command == kWriteHigh)) {
                address_ |= 0x100u;
            }
            if (position == address_bytes_ && session_write_enabled_ && (command == kPageErase || command == kSectorErase)) {
                const uint32_t span = command == kPageErase ? 0x100u : 0x10000u;
                const uint32_t base = address_ & ~(span - 1u);
                for (uint32_t i = 0u; i < span; ++i) {
                    if (uint8_t* byte = At(base + i, false)) {
                        *byte = 0xFFu;
                    }
                }
                dirty_ = true;
                ++revision_;
            }
            return 0xFFu;
        }
        if (command == kReadHigh && !eeprom512 && position == address_bytes_ + 1u) {
            return 0xFFu; // FLASH fast read: dummy byte
        }
        if (command == kRead || command == kReadHigh) {
            const uint8_t* byte = At(address_++, false); // reads run on across pages
            return byte != nullptr ? *byte : 0xFFu;
        }
        if ((command == kWrite || command == kWriteHigh) && session_write_enabled_) {
            // Page writes wrap within their page (16, 32/128, or 256 bytes).
            const uint32_t page = PageSize();
            const uint32_t target = (address_ & ~(page - 1u)) | ((address_ + position - address_bytes_ - 1u) & (page - 1u));
            if (uint8_t* byte = At(target, true)) {
                // FLASH page program only clears bits; EEPROM writes and FLASH page write replace.
                *byte = flash_ && command == kWrite ? static_cast<uint8_t>(*byte & out) : out;
                dirty_ = true;
                ++revision_;
            }
        }
        return 0xFFu;
    }
    default:
        return 0xFFu;
    }
}


uint32_t Backup::PageSize() const noexcept {
    if (address_bytes_ == 1u) {
        return 16u;
    }
    if (address_bytes_ == 2u) {
        return data_.size() > 0x2000u ? 128u : 32u; // 64K EEPROM : 8K EEPROM
    }
    return 256u;
}


uint8_t* Backup::At(uint32_t address, bool grow) {
    const uint32_t limit = address_bytes_ == 1u ? 0x200u : address_bytes_ == 2u ? 0x10000u : kMaxSize;
    if (address >= limit) {
        return nullptr;
    }
    if (address >= data_.size()) {
        if (!grow) {
            return nullptr; // Never written: blank
        }
        size_t size = std::max<size_t>(data_.size(), address_bytes_ == 1u ? 0x200u : address_bytes_ == 2u ? 0x2000u : 0x40000u);
        while (size <= address) {
            size <<= 1u;
        }
        data_.resize(size, 0xFFu);
    }
    return &data_[address];
}


bool Backup::Flush(std::string& error) {
    if (!dirty_ || path_.empty() || data_.empty()) {
        return true;
    }
    // Write a temporary file first so a failure never leaves a truncated save behind.
    std::filesystem::path temporary = path_;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(data_.data()), static_cast<std::streamsize>(data_.size()));
        output.flush();
        output.close();
        if (output.fail()) {
            error = "Failed to write save file: " + temporary.string();
            return false;
        }
    }
    std::error_code rename_error;
    std::filesystem::rename(temporary, path_, rename_error);
    if (rename_error) {
        error = "Failed to replace save file " + path_.string() + ": " + rename_error.message();
        return false;
    }
    dirty_ = false;
    return true;
}

} // namespace emugbds::inds
