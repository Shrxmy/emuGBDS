#include "emugbds/frontend/Session.h"

namespace emugbds::frontend {

Session::Session(std::unique_ptr<cores::InHouseDsCoreAdapter> ds, std::unique_ptr<cores::InHouseGbaCoreAdapter> gba,
                 const runtime::BootConfig& config)
    : ds_(ds.get()), gba_(gba.get()), kernel_(std::move(ds), std::move(gba)), config_(config) {}


std::unique_ptr<Session> Session::Start(const runtime::BootConfig& config, std::string& error) {
    std::unique_ptr<Session> session(new Session(
        std::make_unique<cores::InHouseDsCoreAdapter>(), std::make_unique<cores::InHouseGbaCoreAdapter>(), config));
    if (!session->kernel_.Boot(config, error)) {
        return nullptr;
    }
    return session;
}

} // namespace emugbds::frontend
