#include "App.h"

#include "FileBrowser.h"
#include "emugbds/boot/BootPolicy.h"
#include "emugbds/frontend/Settings.h"
#include "emugbds/input/GbaKeys.h"

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <system_error>
#include <thread>
#include <vector>

namespace emugbds::frontend {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kDsWidth = 256;
constexpr int kDsScreenHeight = 192;
constexpr auto kFrameTarget = std::chrono::duration<double>(1.0 / 59.7275);
// About 60 ms of queued stereo int16 audio: enough to ride out frame jitter.
constexpr uint32_t kAudioQueueTargetBytes = 2048u * 2u * sizeof(int16_t);
constexpr double kStatusSeconds = 4.0;
constexpr const char* kTitle = "emuGBDS " EMUGBDS_VERSION;

struct MemoryRegion {
    const char* name;
    bool arm9; // DS: which CPU's bus to read through
    uint32_t base;
    uint32_t size;
};

constexpr std::array<MemoryRegion, 5> kGbaRegions = {{
    {"EWRAM", true, 0x02000000u, 0x40000u},
    {"IWRAM", true, 0x03000000u, 0x8000u},
    {"Palette", true, 0x05000000u, 0x400u},
    {"VRAM", true, 0x06000000u, 0x18000u},
    {"OAM", true, 0x07000000u, 0x400u},
}};

constexpr std::array<MemoryRegion, 9> kDsRegions = {{
    {"Main RAM", true, 0x02000000u, 0x400000u},
    {"ARM7 WRAM", false, 0x03800000u, 0x10000u},
    {"Palettes", true, 0x05000000u, 0x800u},
    {"Engine A BG VRAM", true, 0x06000000u, 0x80000u},
    {"Engine B BG VRAM", true, 0x06200000u, 0x20000u},
    {"Engine A OBJ VRAM", true, 0x06400000u, 0x40000u},
    {"Engine B OBJ VRAM", true, 0x06600000u, 0x20000u},
    {"LCDC VRAM", true, 0x06800000u, 0xA4000u},
    {"OAM", true, 0x07000000u, 0x800u},
}};

struct SaveChip {
    const char* name;
    uint32_t size;
};

// The DS core picks the chip type from the save file's size (see inds::Backup).
constexpr std::array<SaveChip, 7> kSaveChips = {{
    {"EEPROM 512 B", 0x200u},
    {"EEPROM 8 KB", 0x2000u},
    {"EEPROM 64 KB", 0x10000u},
    {"EEPROM 128 KB", 0x20000u},
    {"FLASH 256 KB", 0x40000u},
    {"FLASH 512 KB", 0x80000u},
    {"FLASH 1 MB", 0x100000u},
}};

constexpr std::array<const char*, 5> kLayerNames = {"BG0", "BG1", "BG2", "BG3", "OBJ"};
constexpr std::array<const char*, 4> kPathLabels = {"GBA BIOS", "DS ARM7 BIOS", "DS ARM9 BIOS", "DS firmware"};
constexpr std::array<int, 5> kFastForwardSpeeds = {2, 3, 4, 8, 0};

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string FileName(const std::string& text) {
    return std::filesystem::u8path(text).filename().u8string();
}

// "screenshots/name.bmp": the folder next to the executable plus the file, short enough for the menu bar.
std::string ShortPath(const std::filesystem::path& path) {
    return (path.parent_path().filename() / path.filename()).u8string();
}

void CopyToBuffer(const std::string& text, char* buffer, size_t size) {
    std::snprintf(buffer, size, "%s", text.c_str());
}

class App {
public:
    App(const AppOptions& options, std::unique_ptr<Session>& session) : options_(options), session_(session) {}
    bool Run(uint64_t& executed_frames, std::string& error);

private:
    enum class Browse { Rom, GbaBios, Bios7, Bios9, Firmware };

    bool InitSdl(std::string& error);
    void ShutdownSdl();
    std::filesystem::path BasePath() const;
    void LoadSettings();
    void SaveSettings();
    void RebuildBindings();
    std::string* PathSetting(int index);

    // Sessions
    void OpenRom(const std::filesystem::path& rom);
    void ResolvePaths(runtime::BootConfig& config) const;
    void Boot(const runtime::BootConfig& config);
    void ResetGame();
    void CloseGame();
    std::string GameName() const;
    std::optional<std::filesystem::path> DsSavePath() const;
    void RecreateSave(const std::filesystem::path& save, uint32_t size);

    // Frame loop
    void HandleEvent(const SDL_Event& event);
    void HandleHotkey(const SDL_KeyboardEvent& key);
    void SendInput();
    void RunEmulation(Clock::time_point frame_begin);
    bool TickOnce(bool queue_audio);
    void UpdateTexture();
    void Render();
    void Pace(Clock::time_point frame_begin, bool emulated, bool fast);

    // Layout
    bool ShowingDs() const noexcept { return texture_height_ == 2u * kDsScreenHeight; }
    int Screens(std::array<SDL_Rect, 2>& src, std::array<SDL_Rect, 2>& dst) const;
    void ResizeWindow();
    void ToggleFullscreen();

    // UI
    void DrawUi();
    void DrawMenuBar();
    void DrawSettingsWindow();
    void DrawControlsWindow();
    void DrawSaveChipWindow();
    void DrawDebugWindow();
    void DrawPopups();
    void ShowStatus(std::string text);
    void ShowError(std::string text);

    // Debug outputs
    void SaveScreenshot();
    void DumpMemory(const MemoryRegion& region);
    std::filesystem::path OutputFile(const char* folder, const std::string& name) const;

    AppOptions options_;
    std::unique_ptr<Session>& session_;
    Settings settings_;
    std::filesystem::path settings_path_;
    std::array<SDL_Scancode, Settings::kButtonCount> bindings_{};
    FileBrowser browser_;
    Browse browse_ = Browse::Rom;

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    SDL_AudioDeviceID audio_device_ = 0;
    bool imgui_ready_ = false;
    bool settings_writable_ = true; // false after a failed load, so the unread file is not overwritten
    uint32_t texture_width_ = 240u;
    uint32_t texture_height_ = 160u;
    bool have_frame_ = false;
    int menu_height_ = 19;
    bool fullscreen_ = false;

    bool quit_ = false;
    bool paused_ = false;
    bool step_ = false;
    bool fast_forward_toggle_ = false;
    bool fast_forward_hold_ = false;
    uint64_t executed_frames_ = 0;

    uint32_t held_mask_ = 0u;
    uint32_t previous_mask_ = 0u;
    bool mouse_down_ = false;
    int mouse_x_ = 0;
    int mouse_y_ = 0;
    bool previous_touch_ = false;
    uint32_t previous_touch_x_ = 0u;
    uint32_t previous_touch_y_ = 0u;

    bool show_settings_ = false;
    bool show_controls_ = false;
    bool show_save_chip_ = false;
    bool show_debug_ = false;
    int capture_button_ = -1;
    std::array<std::array<char, 1024>, 4> path_edit_{};
    int save_chip_choice_ = 4;
    bool confirm_save_chip_ = false;
    bool about_popup_ = false;
    std::filesystem::path confirm_save_path_; // the file the open "Replace save?" dialog is about
    std::array<uint8_t, 3> hidden_layers_{}; // GBA, DS engine A, DS engine B

    std::string status_;
    Clock::time_point status_time_{};
    std::string error_;
    bool error_popup_ = false;
    Clock::time_point fps_time_ = Clock::now();
    uint64_t fps_frames_ = 0;
    double fps_ = 0.0;
};


bool App::Run(uint64_t& executed_frames, std::string& error) {
    if (!InitSdl(error)) {
        ShutdownSdl();
        return false;
    }
    LoadSettings();
    ResizeWindow(); // the saved GBA scale; a DS game resizes again on its first frame
    if (options_.has_rom) {
        runtime::BootConfig config = options_.boot;
        ResolvePaths(config);
        Boot(config);
    }

    while (!quit_) {
        const Clock::time_point frame_begin = Clock::now();
        SDL_Event event{};
        while (SDL_PollEvent(&event) != 0) {
            HandleEvent(event);
        }
        if (quit_) {
            break;
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        menu_height_ = static_cast<int>(ImGui::GetFrameHeight());
        DrawUi();

        const bool fast = fast_forward_toggle_ || fast_forward_hold_;
        const bool emulate = session_ && (!paused_ || step_);
        if (emulate) {
            RunEmulation(frame_begin);
        }
        UpdateTexture();
        Render();
        Pace(frame_begin, emulate && !step_, fast);
        step_ = false;
        if (options_.frame_limit != 0u && executed_frames_ >= options_.frame_limit) {
            quit_ = true;
        }
    }

    executed_frames = executed_frames_;
    SaveSettings();
    ShutdownSdl();
    return true;
}


bool App::InitSdl(std::string& error) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        error = std::string("SDL_Init failed: ") + SDL_GetError();
        return false;
    }
    window_ = SDL_CreateWindow(kTitle, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 240 * 3,
        160 * 3 + menu_height_, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window_) {
        error = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
        return false;
    }
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer_) {
        error = std::string("SDL_CreateRenderer failed: ") + SDL_GetError();
        return false;
    }
    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 240, 160);
    if (!texture_) {
        error = std::string("SDL_CreateTexture failed: ") + SDL_GetError();
        return false;
    }

    // Audio is optional: without a device the loop falls back to the frame timer.
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        SDL_AudioSpec want{};
        want.freq = 32768;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 512;
        audio_device_ = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0); // SDL converts to the device format.
        if (audio_device_ != 0) {
            SDL_PauseAudioDevice(audio_device_, 0);
        }
    }
    if (audio_device_ == 0) {
        std::cerr << "[audio] disabled: " << SDL_GetError() << "\n";
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr; // window positions are not worth a second file
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForSDLRenderer(window_, renderer_);
    ImGui_ImplSDLRenderer2_Init(renderer_);
    imgui_ready_ = true;
    return true;
}


void App::ShutdownSdl() {
    if (imgui_ready_) {
        ImGui_ImplSDLRenderer2_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        imgui_ready_ = false;
    }
    if (audio_device_ != 0) {
        SDL_CloseAudioDevice(audio_device_);
    }
    if (texture_) {
        SDL_DestroyTexture(texture_);
    }
    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
    }
    if (window_) {
        SDL_DestroyWindow(window_);
    }
    SDL_Quit();
}


std::filesystem::path App::BasePath() const {
    char* base = SDL_GetBasePath();
    if (!base) {
        std::error_code error;
        return std::filesystem::current_path(error);
    }
    const std::filesystem::path path = std::filesystem::u8path(base);
    SDL_free(base);
    return path;
}


void App::LoadSettings() {
    settings_path_ = BasePath() / "emugbds.ini";
    std::string error;
    if (!settings_.Load(settings_path_, error)) {
        settings_writable_ = false;
        ShowError(error + ". Changes this session will not be saved.");
    }
    RebuildBindings();
}


void App::SaveSettings() {
    if (!settings_writable_) {
        return;
    }
    std::string error;
    if (!settings_.Save(settings_path_, error)) {
        ShowStatus(error);
        std::cerr << "[settings] " << error << "\n";
    }
}


void App::RebuildBindings() {
    for (size_t i = 0; i < Settings::kButtonCount; ++i) {
        bindings_[i] = SDL_GetScancodeFromName(settings_.keys[i].c_str());
    }
    held_mask_ = 0u; // a held key may no longer map to its button; the release reaches the core next frame
}


std::string* App::PathSetting(int index) {
    switch (index) {
    case 0: return &settings_.gba_bios;
    case 1: return &settings_.nds_bios7;
    case 2: return &settings_.nds_bios9;
    default: return &settings_.nds_firmware;
    }
}


void App::OpenRom(const std::filesystem::path& rom) {
    const std::string extension = Lower(rom.extension().u8string());
    if (extension != ".gba" && extension != ".nds") {
        ShowError("Not a GBA or DS ROM (.gba / .nds): " + rom.u8string());
        return;
    }
    std::error_code exists_error;
    if (!std::filesystem::is_regular_file(rom, exists_error)) {
        ShowError("ROM not found: " + rom.u8string()); // checked first so the running game keeps going
        return;
    }
    runtime::BootConfig config;
    if (extension == ".nds") {
        config.mode_hint = core::ModeHint::DS;
        config.nds_rom_path = rom;
    } else if (settings_.gba_via_ds_firmware) {
        config.mode_hint = core::ModeHint::DS;
        config.slot2_rom_path = rom;
        config.ds_boot_mode = core::DsBootMode::DsFirmware;
    } else {
        config.mode_hint = core::ModeHint::GBA;
        config.gba_rom_path = rom;
    }
    ResolvePaths(config);
    Boot(config);
}


void App::ResolvePaths(runtime::BootConfig& config) const {
    const auto fill = [](std::optional<std::filesystem::path>& path, const std::string& setting) {
        if (!path.has_value() && !setting.empty()) {
            path = std::filesystem::u8path(setting);
        }
    };
    fill(config.gba_bios_path, settings_.gba_bios);
    fill(config.nds_bios7_path, settings_.nds_bios7);
    fill(config.nds_bios9_path, settings_.nds_bios9);
    fill(config.nds_firmware_path, settings_.nds_firmware);
    boot::ResolveDefaultBootPaths(config);
    if (options_.no_gba_bios) {
        config.gba_bios_path.reset();
    }
}


void App::Boot(const runtime::BootConfig& config) {
    // The old session goes first: it writes its save file before the new one reads it.
    session_.reset();
    have_frame_ = false;
    previous_mask_ = 0u;
    previous_touch_ = false;
    if (audio_device_ != 0) {
        SDL_ClearQueuedAudio(audio_device_);
    }
    std::string error;
    session_ = Session::Start(config, error);
    if (!session_) {
        SDL_SetWindowTitle(window_, kTitle);
        ShowError("Could not start the game: " + error);
        return;
    }
    paused_ = false;
    const std::string name = GameName();
    SDL_SetWindowTitle(window_, (std::string(kTitle) + " - " + name).c_str());
    for (const auto* rom : {&config.nds_rom_path, &config.gba_rom_path, &config.slot2_rom_path}) {
        if (rom->has_value()) {
            std::error_code absolute_error;
            const std::filesystem::path absolute = std::filesystem::absolute(rom->value(), absolute_error);
            const std::filesystem::path& stored = absolute_error ? rom->value() : absolute;
            settings_.AddRecentRom(stored.u8string()); // absolute, so the list works from any working folder
            settings_.last_directory = stored.parent_path().u8string();
            break;
        }
    }
    SaveSettings();
}


void App::ResetGame() {
    if (session_) {
        const runtime::BootConfig config = session_->config();
        Boot(config);
        ShowStatus("Reset");
    }
}


void App::CloseGame() {
    session_.reset();
    have_frame_ = false;
    SDL_SetWindowTitle(window_, kTitle);
}


std::string App::GameName() const {
    if (!session_) {
        return {};
    }
    const runtime::BootConfig& config = session_->config();
    for (const auto* rom : {&config.nds_rom_path, &config.gba_rom_path, &config.slot2_rom_path}) {
        if (rom->has_value()) {
            return rom->value().stem().u8string();
        }
    }
    return "game";
}


std::optional<std::filesystem::path> App::DsSavePath() const {
    if (!session_ || !session_->config().nds_rom_path.has_value()) {
        return std::nullopt;
    }
    std::filesystem::path path = *session_->config().nds_rom_path;
    path.replace_extension(".sav"); // as inds::Core::LoadPrimaryRom
    return path;
}


void App::RecreateSave(const std::filesystem::path& save, uint32_t size) {
    // The dialog named this file; a ROM dropped while it was open may have started another game.
    if (DsSavePath() != save) {
        ShowError("A different game is running now; no save was replaced.");
        return;
    }
    const runtime::BootConfig config = session_->config();
    session_.reset(); // writes pending changes, so the backup below is complete
    std::filesystem::path fresh = save;
    fresh += ".new";
    std::error_code error;
    {
        std::ofstream output(fresh, std::ios::binary | std::ios::trunc);
        const std::vector<char> blank(size, static_cast<char>(0xFF)); // an erased chip reads FFh
        output.write(blank.data(), static_cast<std::streamsize>(blank.size()));
        output.close();
        if (output.fail()) {
            std::filesystem::remove(fresh, error);
            ShowError("Could not write " + fresh.u8string() + "; the save was not changed");
            Boot(config);
            return;
        }
    }
    std::filesystem::path backup = save;
    backup += ".bak";
    const bool had_save = std::filesystem::exists(save, error);
    if (had_save) {
        std::filesystem::remove(backup, error);
        std::filesystem::rename(save, backup, error);
        if (error) {
            std::filesystem::remove(fresh, error);
            ShowError("Could not keep the old save as " + backup.u8string() + "; the save was not changed");
            Boot(config);
            return;
        }
    }
    std::filesystem::rename(fresh, save, error);
    if (error) {
        const std::string message = error.message();
        if (had_save) {
            std::filesystem::rename(backup, save, error); // put the old save back
        }
        std::filesystem::remove(fresh, error);
        ShowError("Could not replace " + save.u8string() + ": " + message);
        Boot(config);
        return;
    }
    Boot(config);
    if (session_) {
        ShowStatus("New save chip created; the game restarted");
    }
}


void App::HandleEvent(const SDL_Event& event) {
    ImGui_ImplSDL2_ProcessEvent(&event);
    const ImGuiIO& io = ImGui::GetIO();
    switch (event.type) {
    case SDL_QUIT:
        quit_ = true;
        break;
    case SDL_DROPFILE:
        OpenRom(std::filesystem::u8path(event.drop.file));
        SDL_free(event.drop.file);
        break;
    case SDL_WINDOWEVENT:
        if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            held_mask_ = 0u; // keys released while unfocused never send KEYUP
            mouse_down_ = false;
            fast_forward_hold_ = false;
        }
        break;
    case SDL_KEYDOWN:
        if (capture_button_ >= 0) {
            if (event.key.keysym.scancode != SDL_SCANCODE_ESCAPE) {
                settings_.keys[static_cast<size_t>(capture_button_)] = SDL_GetScancodeName(event.key.keysym.scancode);
                RebuildBindings();
                SaveSettings();
            }
            capture_button_ = -1;
            break;
        }
        if (event.key.repeat != 0 || io.WantCaptureKeyboard) {
            break;
        }
        HandleHotkey(event.key);
        for (size_t i = 0; i < Settings::kButtonCount; ++i) {
            if (bindings_[i] != SDL_SCANCODE_UNKNOWN && bindings_[i] == event.key.keysym.scancode) {
                held_mask_ |= 1u << i;
            }
        }
        break;
    case SDL_KEYUP:
        if (event.key.keysym.scancode == SDL_SCANCODE_TAB) {
            fast_forward_hold_ = false;
        }
        for (size_t i = 0; i < Settings::kButtonCount; ++i) {
            if (bindings_[i] == event.key.keysym.scancode) {
                held_mask_ &= ~(1u << i);
            }
        }
        break;
    case SDL_MOUSEBUTTONDOWN:
        if (event.button.button == SDL_BUTTON_LEFT && !io.WantCaptureMouse) {
            mouse_down_ = true;
            mouse_x_ = event.button.x;
            mouse_y_ = event.button.y;
        }
        break;
    case SDL_MOUSEBUTTONUP:
        if (event.button.button == SDL_BUTTON_LEFT) {
            mouse_down_ = false;
        }
        break;
    case SDL_MOUSEMOTION:
        mouse_x_ = event.motion.x;
        mouse_y_ = event.motion.y;
        break;
    default:
        break;
    }
}


void App::HandleHotkey(const SDL_KeyboardEvent& key) {
    const bool ctrl = (key.keysym.mod & KMOD_CTRL) != 0;
    switch (key.keysym.scancode) {
    case SDL_SCANCODE_ESCAPE:
        quit_ = true;
        break;
    case SDL_SCANCODE_O:
        if (ctrl) {
            browse_ = Browse::Rom;
            browser_.Open("Open ROM", std::filesystem::u8path(settings_.last_directory), {".gba", ".nds"});
        }
        break;
    case SDL_SCANCODE_R:
        if (ctrl) {
            ResetGame();
        }
        break;
    case SDL_SCANCODE_P:
        paused_ = session_ && !paused_;
        break;
    case SDL_SCANCODE_N:
        step_ = paused_;
        break;
    case SDL_SCANCODE_TAB:
        fast_forward_hold_ = true;
        break;
    case SDL_SCANCODE_F1:
        show_debug_ = !show_debug_;
        break;
    case SDL_SCANCODE_F11:
        ToggleFullscreen();
        break;
    case SDL_SCANCODE_F12:
        SaveScreenshot();
        break;
    default:
        break;
    }
}


void App::SendInput() {
    runtime::InputSnapshot input{};
    input.gba_press_mask = held_mask_ & ~previous_mask_;
    input.gba_release_mask = previous_mask_ & ~held_mask_;
    previous_mask_ = held_mask_;
    std::array<SDL_Rect, 2> src{};
    std::array<SDL_Rect, 2> dst{};
    if (mouse_down_ && ShowingDs() && Screens(src, dst) == 2) {
        const SDL_Rect& bottom = dst[1];
        if (bottom.w > 0 && bottom.h > 0 && mouse_x_ >= bottom.x && mouse_x_ < bottom.x + bottom.w &&
            mouse_y_ >= bottom.y && mouse_y_ < bottom.y + bottom.h) {
            input.touch_down = true;
            input.touch_x = static_cast<uint32_t>((mouse_x_ - bottom.x) * kDsWidth / bottom.w);
            input.touch_y = static_cast<uint32_t>((mouse_y_ - bottom.y) * kDsScreenHeight / bottom.h);
        }
    }
    const bool touch_changed = input.touch_down != previous_touch_ ||
        (input.touch_down && (input.touch_x != previous_touch_x_ || input.touch_y != previous_touch_y_));
    previous_touch_ = input.touch_down;
    previous_touch_x_ = input.touch_x;
    previous_touch_y_ = input.touch_y;
    if (input.gba_press_mask != 0u || input.gba_release_mask != 0u || touch_changed) {
        std::string error;
        if (!session_->kernel().InjectInput(input, error)) {
            ShowError("Input failed: " + error);
        }
    }
}


void App::RunEmulation(Clock::time_point frame_begin) {
    session_->gba().SetHiddenLayers(hidden_layers_[0]);
    session_->ds().SetHiddenLayers(0, hidden_layers_[1]);
    session_->ds().SetHiddenLayers(1, hidden_layers_[2]);
    SendInput();

    const bool fast = !step_ && (fast_forward_toggle_ || fast_forward_hold_);
    if (!fast) {
        TickOnce(!step_);
        return;
    }
    if (audio_device_ != 0) {
        SDL_ClearQueuedAudio(audio_device_); // sped-up sound is noise; drop it
    }
    const int speed = settings_.fast_forward_speed;
    for (int i = 0; speed == 0 || i < speed; ++i) {
        if (!TickOnce(false) || (speed == 0 && Clock::now() - frame_begin >= kFrameTarget)) {
            break;
        }
    }
}


bool App::TickOnce(bool queue_audio) {
    runtime::TickResult result{};
    std::string error;
    if (!session_->kernel().Tick(result, error)) {
        paused_ = true;
        ShowError("Emulation stopped: " + error);
        return false;
    }
    if (result.status == runtime::TickStatus::Stopped) {
        paused_ = true;
        ShowStatus("The game stopped");
        return false;
    }
    ++executed_frames_;
    ++fps_frames_;
    if (queue_audio && audio_device_ != 0) {
        runtime::OutputBundle output{};
        session_->kernel().PollOutputs(output);
        if (output.audio.has_value()) {
            if (SDL_GetQueuedAudioSize(audio_device_) > kAudioQueueTargetBytes * 4u) {
                // Uncapped runs or a stalled device outpace playback; drop instead of lagging.
                SDL_ClearQueuedAudio(audio_device_);
            }
            SDL_QueueAudio(audio_device_, output.audio->samples,
                static_cast<uint32_t>(output.audio->sample_count * sizeof(int16_t)));
        }
    }
    return options_.frame_limit == 0u || executed_frames_ < options_.frame_limit;
}


void App::UpdateTexture() {
    if (!session_) {
        return;
    }
    runtime::OutputBundle output{};
    session_->kernel().PollOutputs(output);
    if (!output.video.has_value() || output.video->pixels == nullptr) {
        return;
    }
    const runtime::VideoFrame& frame = *output.video;
    if (frame.width != texture_width_ || frame.height != texture_height_) {
        SDL_Texture* resized = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
            static_cast<int>(frame.width), static_cast<int>(frame.height));
        if (!resized) {
            ShowError(std::string("SDL_CreateTexture failed: ") + SDL_GetError());
            CloseGame();
            return;
        }
        SDL_DestroyTexture(texture_);
        texture_ = resized;
        texture_width_ = frame.width;
        texture_height_ = frame.height;
        ResizeWindow();
    }
    SDL_UpdateTexture(texture_, nullptr, frame.pixels, static_cast<int>(frame.width * sizeof(uint32_t)));
    have_frame_ = true;
}


void App::Render() {
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    if (session_ && have_frame_) {
        std::array<SDL_Rect, 2> src{};
        std::array<SDL_Rect, 2> dst{};
        const int count = Screens(src, dst);
        for (int i = 0; i < count; ++i) {
            SDL_RenderCopy(renderer_, texture_, &src[static_cast<size_t>(i)], &dst[static_cast<size_t>(i)]);
        }
    }
    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer_);
    SDL_RenderPresent(renderer_);

    const auto now = Clock::now();
    const double elapsed = std::chrono::duration<double>(now - fps_time_).count();
    if (elapsed >= 1.0) {
        fps_ = static_cast<double>(fps_frames_) / elapsed;
        fps_frames_ = 0;
        fps_time_ = now;
    }
}


void App::Pace(Clock::time_point frame_begin, bool emulated, bool fast) {
    if (options_.uncapped && emulated) {
        return;
    }
    if (emulated && !fast && audio_device_ != 0 && SDL_GetQueuedAudioSize(audio_device_) > 0u) {
        // The GBA emits 32768 samples per emulated second, so draining to the target paces real time.
        // The deadline keeps a stalled device from freezing input and video.
        const auto deadline = frame_begin + kFrameTarget * 2;
        while (SDL_GetQueuedAudioSize(audio_device_) > kAudioQueueTargetBytes && Clock::now() < deadline) {
            SDL_Delay(1);
        }
        return;
    }
    if (emulated && fast && settings_.fast_forward_speed == 0) {
        return; // unlimited: the batch already filled the frame time
    }
    const auto elapsed = Clock::now() - frame_begin;
    if (elapsed < kFrameTarget) {
        std::this_thread::sleep_for(kFrameTarget - elapsed);
    }
}


int App::Screens(std::array<SDL_Rect, 2>& src, std::array<SDL_Rect, 2>& dst) const {
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(window_, &width, &height);
    const int top = menu_height_;
    const int area_height = std::max(1, height - top);
    const bool ds = ShowingDs();
    const bool side_by_side = ds && settings_.ds_layout == DsLayout::Horizontal;
    const int content_width = side_by_side ? 2 * kDsWidth : static_cast<int>(texture_width_);
    const int content_height = side_by_side ? kDsScreenHeight : static_cast<int>(texture_height_);
    const float scale = std::min(static_cast<float>(width) / static_cast<float>(content_width),
        static_cast<float>(area_height) / static_cast<float>(content_height));
    const int shown_width = static_cast<int>(static_cast<float>(content_width) * scale);
    const int shown_height = static_cast<int>(static_cast<float>(content_height) * scale);
    const int x = (width - shown_width) / 2;
    const int y = top + (area_height - shown_height) / 2;
    if (!ds) {
        src[0] = {0, 0, static_cast<int>(texture_width_), static_cast<int>(texture_height_)};
        dst[0] = {x, y, shown_width, shown_height};
        return 1;
    }
    const int screen_width = side_by_side ? shown_width / 2 : shown_width;
    const int screen_height = side_by_side ? shown_height : shown_height / 2;
    src[0] = {0, 0, kDsWidth, kDsScreenHeight};
    src[1] = {0, kDsScreenHeight, kDsWidth, kDsScreenHeight};
    dst[0] = {x, y, screen_width, screen_height};
    dst[1] = side_by_side ? SDL_Rect{x + screen_width, y, screen_width, screen_height}
                          : SDL_Rect{x, y + screen_height, screen_width, screen_height};
    return 2;
}


void App::ResizeWindow() {
    if (fullscreen_) {
        return;
    }
    const bool ds = ShowingDs();
    const bool side_by_side = ds && settings_.ds_layout == DsLayout::Horizontal;
    const int scale = ds ? settings_.ds_scale : settings_.gba_scale;
    const int width = side_by_side ? 2 * kDsWidth : static_cast<int>(texture_width_);
    const int height = side_by_side ? kDsScreenHeight : static_cast<int>(texture_height_);
    SDL_SetWindowSize(window_, width * scale, height * scale + menu_height_);
}


void App::ToggleFullscreen() {
    fullscreen_ = !fullscreen_;
    SDL_SetWindowFullscreen(window_, fullscreen_ ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0u);
    if (!fullscreen_) {
        ResizeWindow();
    }
}


void App::DrawUi() {
    DrawMenuBar();
    std::filesystem::path chosen;
    if (browser_.Draw(chosen)) {
        if (browse_ == Browse::Rom) {
            OpenRom(chosen);
        } else {
            const int index = static_cast<int>(browse_) - static_cast<int>(Browse::GbaBios);
            *PathSetting(index) = chosen.u8string();
            CopyToBuffer(*PathSetting(index), path_edit_[static_cast<size_t>(index)].data(), path_edit_[0].size());
            SaveSettings();
        }
    }
    DrawSettingsWindow();
    DrawControlsWindow();
    DrawSaveChipWindow();
    DrawDebugWindow();
    DrawPopups();
    if (!session_) {
        const char* hint = "Drop a .gba or .nds file here, or use File > Open ROM (Ctrl+O)";
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2 size = ImGui::CalcTextSize(hint);
        const ImVec2 center = viewport->GetCenter();
        ImGui::GetBackgroundDrawList()->AddText(ImVec2(center.x - size.x * 0.5f, center.y - size.y * 0.5f),
            IM_COL32(200, 200, 200, 255), hint);
    }
}


void App::DrawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    const bool running = session_ != nullptr;
    const bool gba_active = running && session_->kernel().active_mode() == core::EmuMode::GBA;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open ROM...", "Ctrl+O")) {
            browse_ = Browse::Rom;
            browser_.Open("Open ROM", std::filesystem::u8path(settings_.last_directory), {".gba", ".nds"});
        }
        if (ImGui::BeginMenu("Open Recent", !settings_.recent_roms.empty())) {
            std::string pick;
            for (const std::string& rom : settings_.recent_roms) {
                if (ImGui::MenuItem(FileName(rom).c_str())) {
                    pick = rom;
                }
                ImGui::SetItemTooltip("%s", rom.c_str());
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Clear list")) {
                settings_.recent_roms.clear();
                SaveSettings();
            }
            ImGui::EndMenu();
            if (!pick.empty()) {
                OpenRom(std::filesystem::u8path(pick));
            }
        }
        if (ImGui::MenuItem("Close ROM", nullptr, false, running)) {
            CloseGame();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Esc")) {
            quit_ = true;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Emulation")) {
        if (ImGui::MenuItem("Pause", "P", paused_, running)) {
            paused_ = !paused_;
        }
        if (ImGui::MenuItem("Step frame", "N", false, running && paused_)) {
            step_ = true;
        }
        ImGui::MenuItem("Fast-forward", "Tab (hold)", &fast_forward_toggle_);
        if (ImGui::BeginMenu("Fast-forward speed")) {
            for (const int speed : kFastForwardSpeeds) {
                const std::string label = speed == 0 ? std::string("Unlimited") : std::to_string(speed) + "x";
                if (ImGui::MenuItem(label.c_str(), nullptr, settings_.fast_forward_speed == speed)) {
                    settings_.fast_forward_speed = speed;
                    SaveSettings();
                }
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reset", "Ctrl+R", false, running)) {
            ResetGame();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        const bool ds = ShowingDs();
        int& scale = ds ? settings_.ds_scale : settings_.gba_scale;
        if (ImGui::BeginMenu(ds ? "Window size (DS)" : "Window size (GBA)")) {
            for (int s = 1; s <= Settings::kMaxScale; ++s) {
                if (ImGui::MenuItem((std::to_string(s) + "x").c_str(), nullptr, scale == s)) {
                    scale = s;
                    ResizeWindow();
                    SaveSettings();
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("DS screen layout")) {
            const auto layout_item = [&](const char* label, DsLayout layout) {
                if (ImGui::MenuItem(label, nullptr, settings_.ds_layout == layout)) {
                    settings_.ds_layout = layout;
                    if (ShowingDs()) {
                        ResizeWindow();
                    }
                    SaveSettings();
                }
            };
            layout_item("Top above bottom", DsLayout::Vertical);
            layout_item("Side by side", DsLayout::Horizontal);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Fullscreen", "F11", fullscreen_)) {
            ToggleFullscreen();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Settings")) {
        if (ImGui::MenuItem("BIOS & boot...")) {
            show_settings_ = true;
            for (int i = 0; i < 4; ++i) {
                CopyToBuffer(*PathSetting(i), path_edit_[static_cast<size_t>(i)].data(), path_edit_[0].size());
            }
        }
        if (ImGui::MenuItem("Controls...")) {
            show_controls_ = true;
        }
        if (ImGui::MenuItem("DS save chip...", nullptr, false, DsSavePath().has_value())) {
            show_save_chip_ = true;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Debug")) {
        ImGui::MenuItem("CPU / IO panel", "F1", &show_debug_);
        if (ImGui::BeginMenu("Layers")) {
            const auto layer_items = [&](const char* title, uint8_t& hidden) {
                ImGui::SeparatorText(title);
                for (size_t i = 0; i < kLayerNames.size(); ++i) {
                    const uint8_t bit = static_cast<uint8_t>(1u << i);
                    ImGui::PushID(&hidden);
                    if (ImGui::MenuItem(kLayerNames[i], nullptr, (hidden & bit) == 0u)) {
                        hidden = static_cast<uint8_t>(hidden ^ bit);
                    }
                    ImGui::PopID();
                }
            };
            layer_items("GBA", hidden_layers_[0]);
            layer_items("DS engine A", hidden_layers_[1]);
            layer_items("DS engine B", hidden_layers_[2]);
            ImGui::Separator();
            if (ImGui::MenuItem("Show all")) {
                hidden_layers_ = {};
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Screenshot", "F12", false, running && have_frame_)) {
            SaveScreenshot();
        }
        if (ImGui::BeginMenu("Dump memory", running)) {
            if (gba_active) {
                for (const MemoryRegion& region : kGbaRegions) {
                    if (ImGui::MenuItem(region.name)) {
                        DumpMemory(region);
                    }
                }
            } else {
                for (const MemoryRegion& region : kDsRegions) {
                    if (ImGui::MenuItem(region.name)) {
                        DumpMemory(region);
                    }
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About emuGBDS")) {
            about_popup_ = true;
        }
        ImGui::EndMenu();
    }

    // Right side: the latest message, else speed and state.
    std::string right;
    if (!status_.empty() && std::chrono::duration<double>(Clock::now() - status_time_).count() < kStatusSeconds) {
        right = status_;
    } else if (running) {
        char text[64];
        std::snprintf(text, sizeof(text), "%s  %.1f fps", gba_active ? "GBA" : "DS", fps_);
        right = text;
        if (paused_) {
            right += "  paused";
        } else if (fast_forward_toggle_ || fast_forward_hold_) {
            right += "  fast-forward";
        }
    }
    if (!right.empty()) {
        const float width = ImGui::CalcTextSize(right.c_str()).x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - width));
        ImGui::TextUnformatted(right.c_str());
    }
    ImGui::EndMainMenuBar();
}


void App::DrawSettingsWindow() {
    if (!show_settings_) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::Begin("BIOS & boot", &show_settings_)) {
        ImGui::TextWrapped("Leave a path empty to use the file under bios/ in the working folder. "
                           "Changes apply the next time a game starts.");
        for (int i = 0; i < 4; ++i) {
            ImGui::PushID(i);
            ImGui::TextUnformatted(kPathLabels[static_cast<size_t>(i)]);
            ImGui::SetNextItemWidth(-80.0f);
            auto& buffer = path_edit_[static_cast<size_t>(i)];
            ImGui::InputText("##path", buffer.data(), buffer.size());
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                *PathSetting(i) = buffer.data();
                SaveSettings();
            }
            ImGui::SameLine();
            if (ImGui::Button("Browse")) {
                browse_ = static_cast<Browse>(static_cast<int>(Browse::GbaBios) + i);
                const std::filesystem::path current = std::filesystem::u8path(*PathSetting(i));
                browser_.Open(kPathLabels[static_cast<size_t>(i)],
                    current.empty() ? std::filesystem::path("bios") : current.parent_path(), {});
            }
            ImGui::PopID();
        }
        ImGui::Separator();
        if (ImGui::Checkbox("Start .gba files from the DS menu (Slot-2)", &settings_.gba_via_ds_firmware)) {
            SaveSettings();
        }
        ImGui::TextDisabled("With the DS BIOS and firmware set, the real DS menu runs: pick \"Start GBA game\".");
    }
    ImGui::End();
}


void App::DrawControlsWindow() {
    if (!show_controls_) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(380.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::Begin("Controls", &show_controls_)) {
        ImGui::TextWrapped("Click a key, then press the new one (Esc cancels).");
        if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
            for (size_t i = 0; i < Settings::kButtonCount; ++i) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(Settings::kButtonNames[i]);
                ImGui::TableNextColumn();
                ImGui::PushID(static_cast<int>(i));
                const bool capturing = capture_button_ == static_cast<int>(i);
                const std::string label = capturing ? std::string("Press a key...") : settings_.keys[i];
                if (ImGui::Button(label.c_str(), ImVec2(-1.0f, 0.0f))) {
                    capture_button_ = static_cast<int>(i);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (ImGui::Button("Restore defaults")) {
            settings_.keys = Settings{}.keys;
            RebuildBindings();
            SaveSettings();
        }
        ImGui::SeparatorText("Hotkeys");
        ImGui::TextDisabled("Ctrl+O open  Ctrl+R reset  P pause  N step frame\n"
                            "Tab fast-forward  F1 debug panel  F11 fullscreen\n"
                            "F12 screenshot  Esc exit  Mouse = DS touchscreen");
    }
    ImGui::End();
}


void App::DrawSaveChipWindow() {
    if (!show_save_chip_) {
        return;
    }
    const std::optional<std::filesystem::path> save = DsSavePath();
    if (!save.has_value()) {
        show_save_chip_ = false;
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::Begin("DS save chip", &show_save_chip_)) {
        ImGui::TextWrapped("Save file: %s", save->u8string().c_str());
        std::error_code error;
        const bool exists = std::filesystem::exists(*save, error);
        if (exists) {
            const uintmax_t size = std::filesystem::file_size(*save, error);
            const char* name = "unknown size";
            for (const SaveChip& chip : kSaveChips) {
                if (chip.size == size) {
                    name = chip.name;
                }
            }
            ImGui::Text("Chip: %s (%llu bytes)", name, static_cast<unsigned long long>(size));
        } else {
            ImGui::TextWrapped("Chip: not known yet. It is detected from the game's first save, "
                               "which can guess wrong; picking it here avoids that.");
        }
        ImGui::Separator();
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::BeginCombo("##chip", kSaveChips[static_cast<size_t>(save_chip_choice_)].name)) {
            for (int i = 0; i < static_cast<int>(kSaveChips.size()); ++i) {
                if (ImGui::Selectable(kSaveChips[static_cast<size_t>(i)].name, save_chip_choice_ == i)) {
                    save_chip_choice_ = i;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button(exists ? "Replace save with a blank chip..." : "Create blank save...")) {
            confirm_save_chip_ = true;
            confirm_save_path_ = *save;
        }
    }
    ImGui::End();
}


void App::DrawDebugWindow() {
    if (!show_debug_) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(430.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("CPU / IO", &show_debug_)) {
        ImGui::End();
        return;
    }
    const auto registers = [](const char* id, const std::vector<std::pair<const char*, const arm::CpuState*>>& cpus) {
        if (!ImGui::BeginTable(id, static_cast<int>(cpus.size()) + 1, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
            return;
        }
        ImGui::TableSetupColumn("");
        for (const auto& cpu : cpus) {
            ImGui::TableSetupColumn(cpu.first);
        }
        ImGui::TableHeadersRow();
        static constexpr std::array<const char*, 16> kNames = {"R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8",
            "R9", "R10", "R11", "R12", "SP", "LR", "PC"};
        for (size_t r = 0; r <= kNames.size(); ++r) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r < kNames.size() ? kNames[r] : "CPSR");
            for (const auto& cpu : cpus) {
                ImGui::TableNextColumn();
                const uint32_t value = r < kNames.size() ? cpu.second->r[r] : cpu.second->cpsr;
                ImGui::Text("%08X", value);
            }
        }
        ImGui::EndTable();
    };
    const auto unsupported = [](const char* name, const arm::ArmCpu::UnsupportedArmStats& stats) {
        ImGui::TextWrapped("%s unsupported: generic %llu  thumb %llu  multiply %llu  block %llu  swi %llu", name,
            static_cast<unsigned long long>(stats.generic), static_cast<unsigned long long>(stats.thumb),
            static_cast<unsigned long long>(stats.multiply), static_cast<unsigned long long>(stats.block_transfer),
            static_cast<unsigned long long>(stats.software_interrupt));
    };

    if (!session_) {
        ImGui::TextDisabled("No game running.");
    } else if (session_->kernel().active_mode() == core::EmuMode::GBA) {
        const cores::InHouseGbaCoreAdapter& gba = session_->gba();
        ImGui::Text("GBA  frame %llu", static_cast<unsigned long long>(gba.core().frame_counter()));
        ImGui::Text("DISPCNT %04X  DISPSTAT %04X  VCOUNT %u", gba.dispcnt(), gba.dispstat(), gba.vcount());
        ImGui::Text("BGCNT %04X %04X %04X %04X", gba.bgcnt(0), gba.bgcnt(1), gba.bgcnt(2), gba.bgcnt(3));
        ImGui::Text("IME %u  IE %04X  IF %04X", gba.ime(), gba.ie(), gba.iflags());
        unsupported("CPU", gba.unsupported_arm_stats());
        registers("gba_regs", {{"CPU", &gba.core().cpu()}});
    } else {
        const inds::Core& ds = session_->ds().core();
        const inds::Io& io = ds.memory().io();
        ImGui::Text("DS  frame %llu", static_cast<unsigned long long>(ds.frame_counter()));
        ImGui::Text("ARM9  IME %u  IE %08X  IF %08X  %s", io.ime(0), io.ie(0), io.iflags(0),
            ds.arm9_halted() ? "halted" : "running");
        ImGui::Text("ARM7  IME %u  IE %08X  IF %08X  %s", io.ime(1), io.ie(1), io.iflags(1),
            ds.arm7_halted() ? "halted" : "running");
        unsupported("ARM9", ds.arm9_cpu().unsupported_arm_stats());
        unsupported("ARM7", ds.arm7_cpu().unsupported_arm_stats());
        registers("ds_regs", {{"ARM9", &ds.arm9()}, {"ARM7", &ds.arm7()}});
    }
    ImGui::End();
}


void App::DrawPopups() {
    if (about_popup_) {
        ImGui::OpenPopup("About emuGBDS");
        about_popup_ = false;
    }
    if (ImGui::BeginPopupModal("About emuGBDS", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        SDL_version sdl{};
        SDL_GetVersion(&sdl);
        ImGui::Text("%s", kTitle);
        ImGui::TextUnformatted("Clean-room Game Boy Advance and Nintendo DS emulator.");
        ImGui::Separator();
        ImGui::TextDisabled("SDL %u.%u.%u, Dear ImGui %s", sdl.major, sdl.minor, sdl.patch, IMGUI_VERSION);
        if (ImGui::Button("OK", ImVec2(120.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (error_popup_) {
        ImGui::OpenPopup("Error");
        error_popup_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(440.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Error", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextWrapped("%s", error_.c_str());
        if (ImGui::Button("OK", ImVec2(120.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (confirm_save_chip_) {
        ImGui::OpenPopup("Replace save?");
        confirm_save_chip_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(440.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Replace save?", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        const SaveChip& chip = kSaveChips[static_cast<size_t>(save_chip_choice_)];
        ImGui::TextWrapped("The game restarts with a blank %s. An existing save file is kept as .sav.bak "
                           "(replacing any older .sav.bak).", chip.name);
        if (ImGui::Button("Replace and restart")) {
            ImGui::CloseCurrentPopup();
            RecreateSave(confirm_save_path_, chip.size);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}


void App::ShowStatus(std::string text) {
    status_ = std::move(text);
    status_time_ = Clock::now();
}


void App::ShowError(std::string text) {
    std::cerr << "[emugbds] " << text << "\n";
    error_ = std::move(text);
    error_popup_ = true;
}


std::filesystem::path App::OutputFile(const char* folder, const std::string& name) const {
    const std::filesystem::path directory = BasePath() / folder;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return directory / std::filesystem::u8path(name);
}


void App::SaveScreenshot() {
    if (!session_ || !have_frame_) {
        return;
    }
    runtime::OutputBundle output{};
    session_->kernel().PollOutputs(output);
    if (!output.video.has_value() || output.video->pixels == nullptr) {
        return;
    }
    const runtime::VideoFrame& frame = *output.video;
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
    std::filesystem::path path = OutputFile("screenshots", GameName() + "-" + stamp + ".bmp");
    for (int n = 2; std::filesystem::exists(path); ++n) {
        path = OutputFile("screenshots", GameName() + "-" + stamp + "-" + std::to_string(n) + ".bmp");
    }
    SDL_Surface* source = SDL_CreateRGBSurfaceWithFormatFrom(const_cast<uint32_t*>(frame.pixels),
        static_cast<int>(frame.width), static_cast<int>(frame.height), 32, static_cast<int>(frame.width * 4u),
        SDL_PIXELFORMAT_ABGR8888);
    SDL_Surface* rgb = source ? SDL_ConvertSurfaceFormat(source, SDL_PIXELFORMAT_RGB24, 0) : nullptr;
    const bool saved = rgb && SDL_SaveBMP(rgb, path.u8string().c_str()) == 0;
    SDL_FreeSurface(rgb);
    SDL_FreeSurface(source);
    if (saved) {
        ShowStatus("Saved " + ShortPath(path));
    } else {
        ShowError(std::string("Screenshot failed: ") + SDL_GetError());
    }
}


void App::DumpMemory(const MemoryRegion& region) {
    if (!session_) {
        return;
    }
    const bool gba = session_->kernel().active_mode() == core::EmuMode::GBA;
    std::vector<char> bytes(region.size);
    for (uint32_t i = 0; i < region.size; ++i) {
        const uint32_t address = region.base + i;
        bytes[i] = static_cast<char>(gba ? session_->gba().core().bus().Read8(address)
                                         : session_->ds().core().memory().DebugRead8(region.arm9, address));
    }
    std::string name = Lower(region.name);
    std::replace(name.begin(), name.end(), ' ', '-');
    const std::filesystem::path path = OutputFile("dumps", GameName() + "-" + name + ".bin");
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    if (output.fail()) {
        ShowError("Could not write " + path.u8string());
        return;
    }
    char base[16];
    std::snprintf(base, sizeof(base), "%08X", region.base);
    ShowStatus("Dumped " + std::string(region.name) + " (" + base + ") to " + ShortPath(path));
}

} // namespace


bool RunApp(const AppOptions& options, std::unique_ptr<Session>& session, uint64_t& executed_frames, std::string& error) {
    App app(options, session);
    return app.Run(executed_frames, error);
}

} // namespace emugbds::frontend
