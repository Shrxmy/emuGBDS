#include "emugbds/inds/GameCard.h"

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
constexpr uint32_t kRomCtrlRelease = 1u << 29;
constexpr uint32_t kEmptySlotData = 0xFFFFFFFFu;

} // namespace

void GameCard::Reset() noexcept {
    *this = GameCard{};
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
        if ((mask & 0x00FF0000u) != 0u) {
            auxspidata_ = 0xFFu; // Writing starts an SPI exchange; an empty slot answers FFh at once.
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
    if (words_remaining_ == 0u) {
        FinishTransfer();
    } else {
        romctrl_ |= kDataReady;
    }
}


uint32_t GameCard::ReadData() noexcept {
    if (words_remaining_ == 0u) {
        return kEmptySlotData;
    }
    if (--words_remaining_ == 0u) {
        FinishTransfer();
    }
    return kEmptySlotData;
}


void GameCard::FinishTransfer() noexcept {
    romctrl_ &= ~(kBusy | kDataReady);
    if ((auxspicnt_ & kAuxSpiTransferIrq) != 0u) {
        transfer_irq_ = true;
    }
}

} // namespace emugbds::inds
