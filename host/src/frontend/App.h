#pragma once

// The interactive SDL2 + Dear ImGui window: menus, ROM picker and drag-and-drop,
// pause / frame step / fast-forward, window and screen layout, settings, and
// debug panels (CPU/IO state, layer toggles, screenshots, memory dumps).

#include "emugbds/frontend/Session.h"
#include "emugbds/runtime/Types.h"

#include <cstdint>
#include <memory>
#include <string>

namespace emugbds::frontend {

struct AppOptions {
    runtime::BootConfig boot; // from the command line; BIOS paths not resolved yet
    bool has_rom = false;     // boot `boot` when the window opens
    bool no_gba_bios = false;
    uint64_t frame_limit = 0; // 0 = run until the window closes
    bool uncapped = false;
};

// Runs until the window closes. On return `session` holds the last session (for reports).
bool RunApp(const AppOptions& options, std::unique_ptr<Session>& session, uint64_t& executed_frames, std::string& error);

} // namespace emugbds::frontend
