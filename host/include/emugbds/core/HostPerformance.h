#pragma once

namespace emugbds::core {

// Asks the OS to run the calling thread at full speed: on Windows it opts the process out of power
// throttling (EcoQoS) and, on hybrid CPUs, keeps the thread on the performance cores, where the
// emulator runs about 25% faster than when it is moved to efficiency cores. Does nothing elsewhere.
void PreferPerformanceCores() noexcept;

} // namespace emugbds::core
