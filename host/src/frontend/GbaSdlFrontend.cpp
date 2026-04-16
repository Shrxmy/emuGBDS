#include "emugbds/frontend/GbaSdlFrontend.h"
#include "emugbds/input/GbaKeys.h"
#include "emugbds/scheduler/FrameScheduler.h"

#if EMUGBDS_HAS_SDL2 && EMUGBDS_HAS_MGBA
#include <SDL.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {

uint32_t KeyMaskFor(SDL_Keycode key) {
    switch (key) {
    case SDLK_x:
        return emugbds::input::GbaKeyMask(emugbds::input::GbaKey::A);
    case SDLK_z:
        return emugbds::input::GbaKeyMask(emugbds::input::GbaKey::B);
    case SDLK_a:
        return emugbds::input::GbaKeyMask(emugbds::input::GbaKey::L);
    case SDLK_s:
        return emugbds::input::GbaKeyMask(emugbds::input::GbaKey::R);
    case SDLK_RETURN:
        return emugbds::input::GbaKeyMask(emugbds::input::GbaKey::Start);
    case SDLK_BACKSPACE:
        return emugbds::input::GbaKeyMask(emugbds::input::GbaKey::Select);
    case SDLK_UP:
        return emugbds::input::GbaKeyMask(emugbds::input::GbaKey::Up);
    case SDLK_DOWN:
        return emugbds::input::GbaKeyMask(emugbds::input::GbaKey::Down);
    case SDLK_LEFT:
        return emugbds::input::GbaKeyMask(emugbds::input::GbaKey::Left);
    case SDLK_RIGHT:
        return emugbds::input::GbaKeyMask(emugbds::input::GbaKey::Right);
    default:
        return 0;
    }
}

bool RenderFrame(
    SDL_Renderer* renderer,
    SDL_Texture* texture,
    const emugbds::runtime::VideoFrame& frame,
    std::string& error) {
    if (!frame.pixels || frame.width == 0 || frame.height == 0) {
        error = "Runtime video frame is missing pixel data";
        return false;
    }

    if (SDL_UpdateTexture(
            texture,
            nullptr,
            frame.pixels,
            static_cast<int>(frame.width * sizeof(uint32_t))) != 0) {
        error = std::string("SDL texture update failed: ") + SDL_GetError();
        return false;
    }

    SDL_RenderClear(renderer);
    if (SDL_RenderCopy(renderer, texture, nullptr, nullptr) != 0) {
        error = std::string("SDL render copy failed: ") + SDL_GetError();
        return false;
    }
    SDL_RenderPresent(renderer);
    return true;
}

} // namespace
#endif

namespace emugbds::frontend {

bool RunGbaSdlFrontend(runtime::RuntimeKernel& kernel, int scale, uint64_t max_frames, double speed_multiplier, std::string& error) {
#if !(EMUGBDS_HAS_SDL2 && EMUGBDS_HAS_MGBA)
    (void)kernel;
    (void)scale;
    (void)max_frames;
    (void)speed_multiplier;
    error = "SDL frontend is not available in this build";
    return false;
#else
    if (kernel.active_mode() != core::EmuMode::GBA) {
        error = "SDL frontend requires active GBA mode";
        return false;
    }

    if (scale < 1) {
        scale = 1;
    }

#if defined(_WIN32)
    if (std::getenv("SDL_AUDIODRIVER") == nullptr) {
        _putenv_s("SDL_AUDIODRIVER", "directsound");
    }
#endif

    runtime::TickResult tick_result{};
    if (!kernel.Tick(tick_result, error)) {
        return false;
    }
    if (!tick_result.output.video.has_value()) {
        error = "Runtime did not produce a video frame in GBA mode";
        return false;
    }

    const runtime::VideoFrame first_frame = *tick_result.output.video;
    if (!first_frame.pixels || first_frame.width == 0 || first_frame.height == 0) {
        error = "Runtime produced an invalid first video frame";
        return false;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        error = std::string("SDL init failed: ") + SDL_GetError();
        return false;
    }

    SDL_Window* window = SDL_CreateWindow(
        "emuGBDS (Runtime SDL)",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        static_cast<int>(first_frame.width) * scale,
        static_cast<int>(first_frame.height) * scale,
        SDL_WINDOW_SHOWN);
    if (!window) {
        error = std::string("SDL window creation failed: ") + SDL_GetError();
        SDL_Quit();
        return false;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer) {
        error = std::string("SDL renderer creation failed: ") + SDL_GetError();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return false;
    }

    SDL_Texture* texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_ABGR8888,
        SDL_TEXTUREACCESS_STREAMING,
        static_cast<int>(first_frame.width),
        static_cast<int>(first_frame.height));
    if (!texture) {
        error = std::string("SDL texture creation failed: ") + SDL_GetError();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return false;
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);

    SDL_AudioSpec want{};
    SDL_AudioSpec got{};
    want.freq = tick_result.output.audio.has_value() ? static_cast<int>(tick_result.output.audio->sample_rate) : 44100;
    want.format = AUDIO_S16SYS;
    want.channels = static_cast<Uint8>(tick_result.output.audio.has_value() ? tick_result.output.audio->channels : 2);
    want.samples = 1024;
    want.callback = nullptr;

    SDL_AudioDeviceID audio_device = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
    if (audio_device == 0) {
        error = std::string("SDL audio device open failed: ") + SDL_GetError();
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return false;
    }
    SDL_PauseAudioDevice(audio_device, 0);
    const Uint32 max_queued_audio_bytes = static_cast<Uint32>((got.freq * got.channels * sizeof(int16_t)) / 5);

    const double target_fps = 59.7275005696;
    if (speed_multiplier <= 0.0) {
        speed_multiplier = 1.0;
    }
    scheduler::FrameScheduler scheduler;
    if (!scheduler.Configure(target_fps, speed_multiplier, error)) {
        SDL_CloseAudioDevice(audio_device);
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return false;
    }
    scheduler.Start();

    bool saw_nonzero_frame = false;
    uint64_t frame_counter = 1;
    if (!RenderFrame(renderer, texture, first_frame, error)) {
        SDL_CloseAudioDevice(audio_device);
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return false;
    }

    bool running = true;
    while (running) {
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_QUIT:
                running = false;
                break;
            case SDL_KEYDOWN:
                if (event.key.keysym.sym == SDLK_ESCAPE) {
                    running = false;
                    break;
                }
                if (!event.key.repeat) {
                    const uint32_t key = KeyMaskFor(event.key.keysym.sym);
                    if (key != 0) {
                        runtime::InputSnapshot input{};
                        input.gba_press_mask = key;
                        if (!kernel.InjectInput(input, error)) {
                            running = false;
                            break;
                        }
                    }
                }
                break;
            case SDL_KEYUP: {
                const uint32_t key = KeyMaskFor(event.key.keysym.sym);
                if (key != 0) {
                    runtime::InputSnapshot input{};
                    input.gba_release_mask = key;
                    if (!kernel.InjectInput(input, error)) {
                        running = false;
                    }
                }
                break;
            }
            default:
                break;
            }
        }

        if (!running) {
            break;
        }
        if (max_frames > 0 && frame_counter >= max_frames) {
            break;
        }

        if (!kernel.Tick(tick_result, error)) {
            running = false;
            break;
        }
        ++frame_counter;

        if (tick_result.output.active_mode != core::EmuMode::GBA) {
            error = "Runtime switched away from GBA mode while SDL GBA frontend is active";
            running = false;
            break;
        }

        if (tick_result.output.video.has_value()) {
            const runtime::VideoFrame& frame = *tick_result.output.video;
            if (!saw_nonzero_frame && frame_counter <= 600 && frame.pixels) {
                const uint64_t total_pixels = static_cast<uint64_t>(frame.width) * frame.height;
                for (uint64_t i = 0; i < total_pixels; ++i) {
                    if ((frame.pixels[i] & 0x00FFFFFFu) != 0) {
                        saw_nonzero_frame = true;
                        break;
                    }
                }
                if (!saw_nonzero_frame && (frame_counter % 120u) == 0u) {
                    std::cout << "[video] frame " << frame_counter << " is still fully black\n";
                }
                if (!saw_nonzero_frame && frame_counter == 600) {
                    error = "Video frames stayed fully black for 600 frames";
                    running = false;
                    break;
                }
            }

            if (!RenderFrame(renderer, texture, frame, error)) {
                running = false;
                break;
            }
        }

        if (tick_result.output.audio.has_value()) {
            const runtime::AudioChunk& audio = *tick_result.output.audio;
            if (audio.samples && audio.sample_count > 0) {
                if (SDL_GetQueuedAudioSize(audio_device) > max_queued_audio_bytes) {
                    SDL_ClearQueuedAudio(audio_device);
                }
                SDL_QueueAudio(
                    audio_device,
                    audio.samples,
                    static_cast<Uint32>(audio.sample_count * sizeof(int16_t)));
            }
        }

        scheduler.WaitForNextFrame();
    }

    SDL_CloseAudioDevice(audio_device);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    if (!saw_nonzero_frame && error.empty()) {
        error = "No non-zero pixels were produced in the first frames";
        return false;
    }

    return error.empty();
#endif
}

} // namespace emugbds::frontend
