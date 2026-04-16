#pragma once

#include <cstddef>
#include <cstdint>

#if EMUGBDS_HAS_MELONDS
#include "Platform.h"
#else
namespace melonDS::Platform {
enum StopReason {
    Unknown = 0,
};
}
#endif

namespace emugbds::nds {

class IPlatformEvents {
public:
    virtual ~IPlatformEvents() = default;

    virtual void OnSignalStop(melonDS::Platform::StopReason reason) = 0;
    virtual void OnWriteNdsSave(const uint8_t* data, std::size_t size) = 0;
    virtual void OnWriteGbaSave(const uint8_t* data, std::size_t size) = 0;
    virtual void OnWriteFirmware(const uint8_t* data, std::size_t size) = 0;
};

} // namespace emugbds::nds
