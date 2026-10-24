#include "emugbds/inds/MathUnit.h"

#include <cstdint>

namespace emugbds::inds {
namespace {

constexpr uint32_t kDivCnt = 0x00u;
constexpr uint32_t kNumer = 0x10u;
constexpr uint32_t kDenom = 0x18u;
constexpr uint32_t kResult = 0x20u;
constexpr uint32_t kRemainder = 0x28u;
constexpr uint32_t kSqrtCnt = 0x30u;
constexpr uint32_t kSqrtResult = 0x34u;
constexpr uint32_t kSqrtParam = 0x38u;
constexpr uint16_t kDivByZero = 1u << 14;
constexpr uint64_t kUpperHalf = 0xFFFFFFFF00000000ull;

// The low (base) or high (base + 4) word of a 64-bit register.
uint32_t Half(uint64_t value, uint32_t offset, uint32_t base) noexcept {
    return static_cast<uint32_t>(value >> (offset == base ? 0u : 32u));
}

void MergeHalf(uint64_t& target, uint32_t offset, uint32_t base, uint32_t value, uint32_t mask) noexcept {
    const uint32_t shift = offset == base ? 0u : 32u;
    const uint64_t wide_mask = static_cast<uint64_t>(mask) << shift;
    target = (target & ~wide_mask) | ((static_cast<uint64_t>(value) << shift) & wide_mask);
}

} // namespace

uint32_t MathUnit::ReadWord(uint32_t offset) const noexcept {
    switch (offset) {
    case kDivCnt: return divcnt_;
    case kNumer: case kNumer + 4u: return Half(numer_, offset, kNumer);
    case kDenom: case kDenom + 4u: return Half(denom_, offset, kDenom);
    case kResult: case kResult + 4u: return Half(result_, offset, kResult);
    case kRemainder: case kRemainder + 4u: return Half(remainder_, offset, kRemainder);
    case kSqrtCnt: return sqrtcnt_;
    case kSqrtResult: return sqrt_result_;
    case kSqrtParam: case kSqrtParam + 4u: return Half(sqrt_param_, offset, kSqrtParam);
    default: return 0u;
    }
}


void MathUnit::WriteWord(uint32_t offset, uint32_t value, uint32_t mask) noexcept {
    switch (offset) {
    case kDivCnt:
        divcnt_ = static_cast<uint16_t>((divcnt_ & ~(mask & 3u)) | (value & mask & 3u));
        Divide();
        return;
    case kNumer: case kNumer + 4u:
        MergeHalf(numer_, offset, kNumer, value, mask);
        Divide();
        return;
    case kDenom: case kDenom + 4u:
        MergeHalf(denom_, offset, kDenom, value, mask);
        Divide();
        return;
    case kSqrtCnt:
        sqrtcnt_ = static_cast<uint16_t>((sqrtcnt_ & ~(mask & 1u)) | (value & mask & 1u));
        SquareRoot();
        return;
    case kSqrtParam: case kSqrtParam + 4u:
        MergeHalf(sqrt_param_, offset, kSqrtParam, value, mask);
        SquareRoot();
        return;
    default:
        return; // Results are read-only.
    }
}


void MathUnit::Divide() noexcept {
    // The DIV0 flag looks at the full 64-bit denominator, even in 32-bit mode.
    divcnt_ = static_cast<uint16_t>((divcnt_ & ~kDivByZero) | (denom_ == 0u ? kDivByZero : 0u));
    const uint32_t mode = divcnt_ & 3u;
    if (mode == 0u) {
        const int32_t numer = static_cast<int32_t>(static_cast<uint32_t>(numer_));
        const int32_t denom = static_cast<int32_t>(static_cast<uint32_t>(denom_));
        if (denom == 0) {
            // REMAIN=NUMER, RESULT=+/-1 opposite NUMER's sign; overflows invert the upper result half.
            remainder_ = static_cast<uint64_t>(static_cast<int64_t>(numer));
            result_ = static_cast<uint64_t>(static_cast<int64_t>(numer < 0 ? 1 : -1)) ^ kUpperHalf;
        } else if (numer == INT32_MIN && denom == -1) {
            result_ = static_cast<uint64_t>(static_cast<int64_t>(INT32_MIN)) ^ kUpperHalf;
            remainder_ = 0u;
        } else {
            result_ = static_cast<uint64_t>(static_cast<int64_t>(numer / denom));
            remainder_ = static_cast<uint64_t>(static_cast<int64_t>(numer % denom));
        }
        return;
    }
    const int64_t numer = static_cast<int64_t>(numer_);
    // Mode 3 behaves like mode 1 (64/32).
    const int64_t denom = mode == 2u ? static_cast<int64_t>(denom_) : static_cast<int32_t>(static_cast<uint32_t>(denom_));
    if (denom == 0) {
        remainder_ = static_cast<uint64_t>(numer);
        result_ = static_cast<uint64_t>(numer < 0 ? int64_t{1} : int64_t{-1});
    } else if (numer == INT64_MIN && denom == -1) {
        result_ = static_cast<uint64_t>(INT64_MIN);
        remainder_ = 0u;
    } else {
        result_ = static_cast<uint64_t>(numer / denom);
        remainder_ = static_cast<uint64_t>(numer % denom);
    }
}


void MathUnit::SquareRoot() noexcept {
    uint64_t value = (sqrtcnt_ & 1u) != 0u ? sqrt_param_ : static_cast<uint32_t>(sqrt_param_);
    // Bit-by-bit integer square root (floor).
    uint64_t root = 0u;
    for (uint64_t bit = 1ull << 62u; bit != 0u; bit >>= 2u) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1u) + bit;
        } else {
            root >>= 1u;
        }
    }
    sqrt_result_ = static_cast<uint32_t>(root);
}

} // namespace emugbds::inds
