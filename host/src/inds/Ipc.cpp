#include "emugbds/inds/Ipc.h"

namespace emugbds::inds {
namespace {

constexpr uint16_t kSendEmptyIrqEnable = 1u << 2;
constexpr uint16_t kSendClear = 1u << 3;
constexpr uint16_t kRecvIrqEnable = 1u << 10;
constexpr uint16_t kError = 1u << 14;
constexpr uint16_t kFifoEnable = 1u << 15;

} // namespace

void Ipc::Reset() noexcept {
    sides_ = {};
    pending_irqs_ = {};
    UpdateFifoIrqs();
    pending_irqs_ = {}; // Power-on conditions do not raise IRQs.
}


uint32_t Ipc::ReadSync(int cpu) const noexcept {
    const Side& self = sides_[cpu];
    return sides_[cpu ^ 1].sync_out | (static_cast<uint32_t>(self.sync_out) << 8u) |
        (self.sync_irq_enable ? (1u << 14) : 0u);
}


void Ipc::WriteSync(int cpu, uint32_t value, uint32_t mask) noexcept {
    Side& self = sides_[cpu];
    if ((mask & 0x0F00u) != 0u) {
        self.sync_out = static_cast<uint8_t>((value >> 8u) & 0xFu);
    }
    if ((mask & (1u << 14)) != 0u) {
        self.sync_irq_enable = (value & (1u << 14)) != 0u;
    }
    if ((mask & value & (1u << 13)) != 0u && sides_[cpu ^ 1].sync_irq_enable) {
        pending_irqs_[cpu ^ 1] |= kSyncIrq;
    }
}


uint32_t Ipc::ReadFifoControl(int cpu) const noexcept {
    const Side& self = sides_[cpu];
    const auto& recv = sides_[cpu ^ 1].send;
    return self.fifo_control |
        (self.send.empty() ? 1u << 0 : 0u) | (self.send.size() == kFifoDepth ? 1u << 1 : 0u) |
        (recv.empty() ? 1u << 8 : 0u) | (recv.size() == kFifoDepth ? 1u << 9 : 0u);
}


void Ipc::WriteFifoControl(int cpu, uint32_t value, uint32_t mask) noexcept {
    Side& self = sides_[cpu];
    const uint16_t writable = static_cast<uint16_t>(mask & (kSendEmptyIrqEnable | kRecvIrqEnable | kFifoEnable));
    self.fifo_control = static_cast<uint16_t>((self.fifo_control & ~writable) | (value & writable));
    if ((mask & value & kError) != 0u) {
        self.fifo_control &= static_cast<uint16_t>(~kError); // Writing 1 acknowledges the error.
    }
    if ((mask & value & kSendClear) != 0u) {
        self.send.clear();
        sides_[cpu ^ 1].last_received = 0u; // A cleared FIFO reads as zero.
    }
    UpdateFifoIrqs();
}


void Ipc::Send(int cpu, uint32_t value) {
    Side& self = sides_[cpu];
    if ((self.fifo_control & kFifoEnable) == 0u) {
        return; // Ignored, without an error.
    }
    if (self.send.size() == kFifoDepth) {
        self.fifo_control |= kError;
        return;
    }
    self.send.push_back(value);
    UpdateFifoIrqs();
}


uint32_t Ipc::Receive(int cpu) noexcept {
    Side& self = sides_[cpu];
    auto& recv = sides_[cpu ^ 1].send;
    if (recv.empty()) {
        self.fifo_control |= kError;
        return self.last_received;
    }
    if ((self.fifo_control & kFifoEnable) == 0u) {
        return recv.front(); // Disabled: peek without removing.
    }
    self.last_received = recv.front();
    recv.pop_front();
    UpdateFifoIrqs();
    return self.last_received;
}


uint32_t Ipc::TakeIrqs(int cpu) noexcept {
    const uint32_t irqs = pending_irqs_[cpu];
    pending_irqs_[cpu] = 0u;
    return irqs;
}


void Ipc::UpdateFifoIrqs() noexcept {
    // Both FIFO IRQs are edge-triggered on their enable-and-condition terms.
    for (int cpu = 0; cpu < 2; ++cpu) {
        Side& self = sides_[cpu];
        const bool send_empty = (self.fifo_control & kSendEmptyIrqEnable) != 0u && self.send.empty();
        const bool recv = (self.fifo_control & kRecvIrqEnable) != 0u && !sides_[cpu ^ 1].send.empty();
        if (send_empty && !self.send_empty_condition) {
            pending_irqs_[cpu] |= kSendEmptyIrq;
        }
        if (recv && !self.recv_condition) {
            pending_irqs_[cpu] |= kRecvNotEmptyIrq;
        }
        self.send_empty_condition = send_empty;
        self.recv_condition = recv;
    }
}

} // namespace emugbds::inds
