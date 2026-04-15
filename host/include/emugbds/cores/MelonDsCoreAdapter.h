#pragma once

#include "emugbds/core/IEmuCore.h"
#include "emugbds/melonds/PlatformEvents.h"

#include <memory>
#include <optional>
#include <vector>

namespace melonDS {
class NDS;
namespace Platform {
enum StopReason;
}
}

namespace emugbds::cores {

class MelonDsCoreAdapter final : public core::IEmuCore, public melonds::IPlatformEvents {
public:
    MelonDsCoreAdapter() = default;
    ~MelonDsCoreAdapter() override;

    core::EmuMode mode() const noexcept override;
    bool Initialize(std::string& error) override;
    bool LoadPrimaryRom(const std::filesystem::path& rom_path, std::string& error) override;
    bool RunFrame(std::string& error) override;
    bool Reset(std::string& error) override;
    bool IsRunning() const noexcept override;
    void Shutdown() noexcept override;

    bool SetSecondaryRom(const std::filesystem::path& rom_path, std::string& error) override;
    std::optional<core::TransitionEvent> PollTransition() override;

    void OnSignalStop(melonDS::Platform::StopReason reason) override;
    void OnWriteNdsSave(const uint8_t* data, std::size_t size) override;
    void OnWriteGbaSave(const uint8_t* data, std::size_t size) override;
    void OnWriteFirmware(const uint8_t* data, std::size_t size) override;

private:
    bool InsertSlot2Rom(std::string& error);

#if EMUGBDS_HAS_MELONDS
    std::unique_ptr<melonDS::NDS> nds_;
#else
    std::unique_ptr<int> nds_;
#endif
    bool primary_loaded_ = false;
    std::optional<std::filesystem::path> primary_rom_path_;
    std::optional<std::filesystem::path> slot2_rom_path_;
    std::optional<core::TransitionEvent> pending_transition_;
    std::vector<uint8_t> nds_save_image_;
    std::vector<uint8_t> gba_save_image_;
    std::vector<uint8_t> firmware_image_;
};

} // namespace emugbds::cores
