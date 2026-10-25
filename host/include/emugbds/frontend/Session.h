#pragma once

// One emulation run: the in-house DS and GBA cores and the runtime kernel that
// switches between them. Destroying a session shuts the cores down, which
// writes any pending DS cartridge save.

#include "emugbds/cores/InHouseDsCoreAdapter.h"
#include "emugbds/cores/InHouseGbaCoreAdapter.h"
#include "emugbds/runtime/RuntimeKernel.h"

#include <memory>
#include <string>

namespace emugbds::frontend {

class Session {
public:
    // Boots `config` (paths already resolved); returns nullptr with `error` on failure.
    static std::unique_ptr<Session> Start(const runtime::BootConfig& config, std::string& error);
    ~Session() { kernel_.Shutdown(); }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    runtime::RuntimeKernel& kernel() noexcept { return kernel_; }
    cores::InHouseDsCoreAdapter& ds() noexcept { return *ds_; }
    cores::InHouseGbaCoreAdapter& gba() noexcept { return *gba_; }
    const runtime::BootConfig& config() const noexcept { return config_; }

private:
    Session(std::unique_ptr<cores::InHouseDsCoreAdapter> ds, std::unique_ptr<cores::InHouseGbaCoreAdapter> gba,
            const runtime::BootConfig& config);

    cores::InHouseDsCoreAdapter* ds_; // owned by kernel_
    cores::InHouseGbaCoreAdapter* gba_;
    runtime::RuntimeKernel kernel_;
    runtime::BootConfig config_;
};

} // namespace emugbds::frontend
