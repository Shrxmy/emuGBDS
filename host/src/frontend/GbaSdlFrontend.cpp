#include "emugbds/frontend/GbaSdlFrontend.h"

#if EMUGBDS_HAS_SDL2 && EMUGBDS_HAS_MGBA
#include <mgba/core/core.h>
#include <mgba/internal/gba/input.h>
#include <mgba-util/audio-buffer.h>
#include <mgba-util/image.h>

#include <SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {

uint32_t KeyMaskFor(SDL_Keycode key) {
    switch (key) {
    case SDLK_x:
        return 1u << GBA_KEY_A;
    case SDLK_z:
        return 1u << GBA_KEY_B;
    case SDLK_a:
        return 1u << GBA_KEY_L;
    case SDLK_s:
        return 1u << GBA_KEY_R;
    case SDLK_RETURN:
        return 1u << GBA_KEY_START;
    case SDLK_BACKSPACE:
        return 1u << GBA_KEY_SELECT;
    case SDLK_UP:
        return 1u << GBA_KEY_UP;
    case SDLK_DOWN:
        return 1u << GBA_KEY_DOWN;
    case SDLK_LEFT:
        return 1u << GBA_KEY_LEFT;
    case SDLK_RIGHT:
        return 1u << GBA_KEY_RIGHT;
    default:
        return 0;
    }
}

Uint32 NativePixelFormat() {
    return SDL_PIXELFORMAT_ABGR8888;
}

} // namespace
#endif

namespace emugbds::frontend {

bool RunGbaSdlFrontend(cores::MGbaCoreAdapter& adapter, int scale, uint64_t max_frames, double speed_multiplier, std::string& error) {
#if !(EMUGBDS_HAS_SDL2 && EMUGBDS_HAS_MGBA)
    (void)adapter;
    (void)scale;
    (void)max_frames;
    (void)speed_multiplier;
    error = "SDL frontend is not available in this build";
    return false;
#else
    mCore* core = adapter.NativeCore();
    if (!core) {
        error = "mGBA core is not initialized";
        return false;
    }

    unsigned width = 0;
    unsigned height = 0;
    core->baseVideoSize(core, &width, &height);
    if (width == 0 || height == 0) {
        error = "Invalid video dimensions from mGBA core";
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

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        error = std::string("SDL init failed: ") + SDL_GetError();
        return false;
    }

    SDL_Window* window = SDL_CreateWindow(
        "emuGBDS (mGBA SDL Test)",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        static_cast<int>(width) * scale,
        static_cast<int>(height) * scale,
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
        NativePixelFormat(),
        SDL_TEXTUREACCESS_STREAMING,
        static_cast<int>(width),
        static_cast<int>(height));
    if (!texture) {
        error = std::string("SDL texture creation failed: ") + SDL_GetError();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return false;
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);

    std::vector<mColor> frame_buffer(width * height);
    std::vector<uint32_t> upload_buffer(width * height);
    core->setVideoBuffer(core, frame_buffer.data(), width);
    if (!adapter.Reset(error)) {
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return false;
    }

    bool saw_nonzero_frame = false;
    uint64_t frame_counter = 0;

    SDL_AudioSpec want{};
    SDL_AudioSpec got{};
    const int sample_rate = static_cast<int>(core->audioSampleRate(core));
    want.freq = sample_rate > 0 ? sample_rate : 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
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
    core->setAudioBufferSize(core, got.samples * 2);
    SDL_PauseAudioDevice(audio_device, 0);

    std::vector<int16_t> audio_samples;
    const uint64_t perf_frequency = SDL_GetPerformanceFrequency();
    uint64_t frame_deadline = SDL_GetPerformanceCounter();
    const uint32_t frame_cycles = core->frameCycles(core);
    const uint32_t cpu_frequency = core->frequency(core);
    double target_fps = 59.7275005696;
    if (frame_cycles > 0 && cpu_frequency > 0) {
        const double derived_fps = static_cast<double>(cpu_frequency) / static_cast<double>(frame_cycles);
        if (derived_fps >= 50.0 && derived_fps <= 70.0) {
            target_fps = derived_fps;
        }
    }
    if (speed_multiplier <= 0.0) {
        speed_multiplier = 1.0;
    }
    target_fps *= speed_multiplier;

    const double safe_target_fps = target_fps > 1.0 ? target_fps : 1.0;
    const double ticks_per_frame = static_cast<double>(perf_frequency) / safe_target_fps;
    const Uint32 max_queued_audio_bytes = static_cast<Uint32>((got.freq * got.channels * sizeof(int16_t)) / 5); // ~200ms
    const int got_freq_chunk = got.freq / 20;
    const size_t max_audio_frames_per_tick = static_cast<size_t>(got_freq_chunk > 1 ? got_freq_chunk : 1); // ~50ms

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
                        core->addKeys(core, key);
                    }
                }
                break;
            case SDL_KEYUP: {
                const uint32_t key = KeyMaskFor(event.key.keysym.sym);
                if (key != 0) {
                    core->clearKeys(core, key);
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
            running = false;
            break;
        }

        if (!adapter.RunFrame(error)) {
            running = false;
            break;
        }
        ++frame_counter;

        if (!saw_nonzero_frame && frame_counter <= 600) {
            for (const mColor pixel : frame_buffer) {
                if (pixel != 0) {
                    saw_nonzero_frame = true;
                    break;
                }
            }
            if (!saw_nonzero_frame && frame_counter == 600) {
                error = "Video buffer stayed fully black for 600 frames after reset";
                running = false;
                break;
            }
        }

        for (size_t i = 0; i < frame_buffer.size(); ++i) {
            const uint32_t converted = mColorConvert(static_cast<uint32_t>(frame_buffer[i]), mCOLOR_NATIVE, mCOLOR_ABGR8);
            upload_buffer[i] = converted | 0xFF000000u;
        }
        if (SDL_UpdateTexture(
                texture,
                nullptr,
                upload_buffer.data(),
                static_cast<int>(width * sizeof(uint32_t))) != 0) {
            error = std::string("SDL texture update failed: ") + SDL_GetError();
            running = false;
            break;
        }

        SDL_RenderClear(renderer);
        if (SDL_RenderCopy(renderer, texture, nullptr, nullptr) != 0) {
            error = std::string("SDL render copy failed: ") + SDL_GetError();
            running = false;
            break;
        }
        SDL_RenderPresent(renderer);

        mAudioBuffer* audio_buffer = core->getAudioBuffer(core);
        if (audio_buffer) {
            if (SDL_GetQueuedAudioSize(audio_device) > max_queued_audio_bytes) {
                SDL_ClearQueuedAudio(audio_device);
            }
            const size_t available_frames = mAudioBufferAvailable(audio_buffer);
            if (available_frames > 0) {
                const size_t frames_to_read = available_frames < max_audio_frames_per_tick
                    ? available_frames
                    : max_audio_frames_per_tick;
                audio_samples.resize(frames_to_read * 2);
                const size_t read_frames = mAudioBufferRead(audio_buffer, audio_samples.data(), frames_to_read);
                if (read_frames > 0) {
                    SDL_QueueAudio(
                        audio_device,
                        audio_samples.data(),
                        static_cast<Uint32>(read_frames * 2 * sizeof(int16_t)));
                }
            }
        }

        frame_deadline += static_cast<uint64_t>(ticks_per_frame);
        const uint64_t now = SDL_GetPerformanceCounter();
        if (frame_deadline > now) {
            const uint64_t ticks_left = frame_deadline - now;
            const uint64_t ms_left = (ticks_left * 1000ULL) / perf_frequency;
            if (ms_left > 2) {
                SDL_Delay(static_cast<Uint32>(ms_left - 1));
            }
            while (SDL_GetPerformanceCounter() < frame_deadline) {
                // Busy-wait for sub-ms accuracy; avoids scheduler oversleep.
            }
        } else {
            const uint64_t max_lag_ticks = static_cast<uint64_t>(ticks_per_frame * 4.0);
            if (now - frame_deadline > max_lag_ticks) {
                frame_deadline = now;
            }
        }
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
