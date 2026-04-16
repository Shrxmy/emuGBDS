#pragma once

#include <cstdint>

namespace emugbds::ingba {

class IBus {
public:
    virtual ~IBus() = default;

    virtual uint8_t Read8(uint32_t address) const = 0;
    virtual uint16_t Read16(uint32_t address) const = 0;
    virtual uint32_t Read32(uint32_t address) const = 0;
    virtual void Write8(uint32_t address, uint8_t value) = 0;
    virtual void Write16(uint32_t address, uint16_t value) = 0;
    virtual void Write32(uint32_t address, uint32_t value) = 0;
};

} // namespace emugbds::ingba
