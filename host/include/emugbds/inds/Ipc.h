#pragma once

// Inter-processor communication between the DS CPUs: IPCSYNC (4000180h),
// IPCFIFOCNT (4000184h), IPCFIFOSEND (4000188h), IPCFIFORECV (4100000h).
// CPU index 0 is the ARM9, 1 is the ARM7.

#include <array>
#include <cstdint>
#include <deque>

namespace emugbds::inds {

class Ipc {
public:
    static constexpr uint32_t kSyncIrq = 1u << 16;
    static constexpr uint32_t kSendEmptyIrq = 1u << 17;
    static constexpr uint32_t kRecvNotEmptyIrq = 1u << 18;

    void Reset() noexcept;
    uint32_t ReadSync(int cpu) const noexcept;
    void WriteSync(int cpu, uint32_t value, uint32_t mask) noexcept;
    uint32_t ReadFifoControl(int cpu) const noexcept;
    void WriteFifoControl(int cpu, uint32_t value, uint32_t mask) noexcept;
    void Send(int cpu, uint32_t value);
    uint32_t Receive(int cpu) noexcept;
    // IF bits raised for this CPU since the last call.
    uint32_t TakeIrqs(int cpu) noexcept;

private:
    static constexpr size_t kFifoDepth = 16u;

    struct Side {
        uint8_t sync_out = 0;          // IPCSYNC bits 8-11, seen by the other CPU as bits 0-3
        bool sync_irq_enable = false;  // IPCSYNC bit 14
        uint16_t fifo_control = 0;     // IPCFIFOCNT bits 2, 10, 14, 15
        std::deque<uint32_t> send;     // This CPU's send FIFO = the other CPU's receive FIFO
        uint32_t last_received = 0;
        bool send_empty_condition = false;
        bool recv_condition = false;
    };

    void UpdateFifoIrqs() noexcept;

    std::array<Side, 2> sides_{};
    std::array<uint32_t, 2> pending_irqs_{};
};

} // namespace emugbds::inds
