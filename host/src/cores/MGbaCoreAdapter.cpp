#include "emugbds/cores/MGbaCoreAdapter.h"

#if EMUGBDS_HAS_MGBA
#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/log.h>
#include <mgba/gba/core.h>
#include <mgba-util/vfs.h>
#include <fcntl.h>
#endif

namespace emugbds::cores {

#if EMUGBDS_HAS_MGBA
namespace {

void SilentMgbaLog(mLogger*, int, enum mLogLevel, const char*, va_list) {}

mLogger g_silent_mgba_logger = {SilentMgbaLog, nullptr};

} // namespace
#endif

MGbaCoreAdapter::~MGbaCoreAdapter() {
    Shutdown();
}

core::EmuMode MGbaCoreAdapter::mode() const noexcept {
    return core::EmuMode::GBA;
}

bool MGbaCoreAdapter::Initialize(std::string& error) {
#if !EMUGBDS_HAS_MGBA
    error = "mGBA support is not compiled in";
    return false;
#else
    if (core_) {
        return true;
    }

    core_ = GBACoreCreate();
    if (!core_) {
        error = "Failed to allocate mGBA core";
        return false;
    }

    mLogSetDefaultLogger(&g_silent_mgba_logger);

    if (!core_->init(core_)) {
        error = "mGBA core initialization failed";
        core_->deinit(core_);
        core_ = nullptr;
        return false;
    }

    mCoreInitConfig(core_, "emugbds");
    mCoreConfigSetDefaultIntValue(&core_->config, "volume", 0x100);
    mCoreConfigSetDefaultIntValue(&core_->config, "mute", 0);
    mCoreConfigSetDefaultUIntValue(&core_->config, "audioBuffers", 1024);
    mCoreConfigSetDefaultValue(&core_->config, "logLevel", "fatal");
    mCoreLoadConfig(core_);

    if (bios_path_.has_value()) {
        mCoreConfigSetOverrideIntValue(&core_->config, "useBios", 1);
        mCoreConfigSetOverrideIntValue(&core_->config, "skipBios", 0);

        VFile* bios_file = VFileOpen(bios_path_->string().c_str(), O_RDONLY);
        if (!bios_file) {
            error = "Failed to open GBA BIOS file: " + bios_path_->string();
            Shutdown();
            return false;
        }

        const bool bios_loaded = core_->loadBIOS(core_, bios_file, 0);
        if (!bios_loaded) {
            bios_file->close(bios_file);
            error = "mGBA failed to load GBA BIOS: " + bios_path_->string();
            Shutdown();
            return false;
        }
    }

    return true;
#endif
}

bool MGbaCoreAdapter::LoadPrimaryRom(const std::filesystem::path& rom_path, std::string& error) {
#if !EMUGBDS_HAS_MGBA
    (void)rom_path;
    error = "mGBA support is not compiled in";
    return false;
#else
    if (!core_ && !Initialize(error)) {
        return false;
    }

    if (loaded_) {
        core_->unloadROM(core_);
        loaded_ = false;
    }

    if (!mCoreLoadFile(core_, rom_path.string().c_str())) {
        error = "mGBA failed to load ROM: " + rom_path.string();
        return false;
    }

    core_->reset(core_);
    loaded_ = true;
    return true;
#endif
}

bool MGbaCoreAdapter::RunFrame(std::string& error) {
#if !EMUGBDS_HAS_MGBA
    error = "mGBA support is not compiled in";
    return false;
#else
    if (!core_ || !loaded_) {
        error = "mGBA core is not ready to run";
        return false;
    }

    core_->runFrame(core_);
    return true;
#endif
}

bool MGbaCoreAdapter::Reset(std::string& error) {
#if !EMUGBDS_HAS_MGBA
    error = "mGBA support is not compiled in";
    return false;
#else
    if (!core_ || !loaded_) {
        error = "mGBA core is not ready to reset";
        return false;
    }
    core_->reset(core_);
    return true;
#endif
}

bool MGbaCoreAdapter::IsRunning() const noexcept {
    return core_ != nullptr && loaded_;
}

void MGbaCoreAdapter::Shutdown() noexcept {
#if EMUGBDS_HAS_MGBA
    if (!core_) {
        return;
    }

    if (loaded_) {
        core_->unloadROM(core_);
        loaded_ = false;
    }

    mCoreConfigDeinit(&core_->config);
    core_->deinit(core_);
    core_ = nullptr;
#endif
}

bool MGbaCoreAdapter::SetBiosPath(const std::filesystem::path& bios_path, std::string& error) {
#if !EMUGBDS_HAS_MGBA
    (void)bios_path;
    error = "mGBA support is not compiled in";
    return false;
#else
    if (!std::filesystem::exists(bios_path)) {
        error = "GBA BIOS file does not exist: " + bios_path.string();
        return false;
    }
    bios_path_ = bios_path;
    return true;
#endif
}

} // namespace emugbds::cores
