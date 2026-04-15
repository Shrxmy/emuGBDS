#include "emugbds/melonds/PlatformEvents.h"

#if EMUGBDS_HAS_MELONDS
#include "Platform.h"
#include "SPI_Firmware.h"
#endif

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <chrono>
#include <condition_variable>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

#if EMUGBDS_HAS_MELONDS
namespace {

using namespace melonDS;
using namespace melonDS::Platform;

constexpr bool HasMode(FileMode mode, FileMode flag) {
    return (static_cast<unsigned>(mode) & static_cast<unsigned>(flag)) != 0;
}

char AccessMode(FileMode mode, bool file_exists) {
    if (HasMode(mode, FileMode::Append)) {
        return 'a';
    }
    if (!HasMode(mode, FileMode::Write)) {
        return 'r';
    }
    if (HasMode(mode, FileMode::NoCreate)) {
        return 'r';
    }
    if (HasMode(mode, FileMode::Preserve) && file_exists) {
        return 'r';
    }
    return 'w';
}

bool IsExtended(FileMode mode) {
    return HasMode(mode, FileMode::Read) && HasMode(mode, FileMode::Write);
}

std::string BuildOpenMode(FileMode mode, bool file_exists) {
    std::string mode_string;
    mode_string += AccessMode(mode, file_exists);
    if (IsExtended(mode)) {
        mode_string += '+';
    }
    if (!HasMode(mode, FileMode::Text)) {
        mode_string += 'b';
    }
    return mode_string;
}

emugbds::melonds::IPlatformEvents* AsEvents(void* userdata) {
    return reinterpret_cast<emugbds::melonds::IPlatformEvents*>(userdata);
}

struct ThreadImpl {
    explicit ThreadImpl(std::function<void()> fn) : thread(std::move(fn)) {}
    std::thread thread;
};

struct SemaphoreImpl {
    std::mutex mutex;
    std::condition_variable condition;
    int count = 0;
};

struct MutexImpl {
    std::mutex mutex;
};

struct AACDecoder {
    int unused = 0;
};

struct DynamicLibrary {
#if defined(_WIN32)
    HMODULE handle = nullptr;
#else
    void* handle = nullptr;
#endif
};

const auto kStart = std::chrono::steady_clock::now();

} // namespace

namespace melonDS::Platform {

void SignalStop(StopReason reason, void* userdata) {
    if (auto* events = AsEvents(userdata)) {
        events->OnSignalStop(reason);
    }
}

std::string GetLocalFilePath(const std::string& filename) {
    std::filesystem::path path(filename);
    if (path.is_absolute()) {
        return path.string();
    }
    return (std::filesystem::current_path() / path).string();
}

FileHandle* OpenFile(const std::string& path, FileMode mode) {
    if (!HasMode(mode, FileMode::Read) && !HasMode(mode, FileMode::Write) && !HasMode(mode, FileMode::Append)) {
        return nullptr;
    }

    const bool file_exists = std::filesystem::exists(std::filesystem::path(path));
    const std::string mode_string = BuildOpenMode(mode, file_exists);

    FILE* file = nullptr;
#if defined(_WIN32)
    if (fopen_s(&file, path.c_str(), mode_string.c_str()) != 0) {
        file = nullptr;
    }
#else
    file = std::fopen(path.c_str(), mode_string.c_str());
#endif
    return reinterpret_cast<FileHandle*>(file);
}

FileHandle* OpenLocalFile(const std::string& path, FileMode mode) {
    return OpenFile(GetLocalFilePath(path), mode);
}

bool FileExists(const std::string& name) {
    return std::filesystem::exists(std::filesystem::path(name));
}

bool LocalFileExists(const std::string& name) {
    return FileExists(GetLocalFilePath(name));
}

bool CheckFileWritable(const std::string& filepath) {
    FileHandle* file = OpenFile(filepath, FileMode::Append);
    if (!file) {
        return false;
    }
    return CloseFile(file);
}

bool CheckLocalFileWritable(const std::string& name) {
    return CheckFileWritable(GetLocalFilePath(name));
}

bool CloseFile(FileHandle* file) {
    if (!file) {
        return false;
    }
    return std::fclose(reinterpret_cast<FILE*>(file)) == 0;
}

bool IsEndOfFile(FileHandle* file) {
    if (!file) {
        return true;
    }
    return std::feof(reinterpret_cast<FILE*>(file)) != 0;
}

bool FileReadLine(char* str, int count, FileHandle* file) {
    if (!str || !file || count <= 0) {
        return false;
    }
    return std::fgets(str, count, reinterpret_cast<FILE*>(file)) != nullptr;
}

u64 FilePosition(FileHandle* file) {
    if (!file) {
        return 0;
    }
#if defined(_WIN32)
    return static_cast<u64>(_ftelli64(reinterpret_cast<FILE*>(file)));
#else
    return static_cast<u64>(std::ftell(reinterpret_cast<FILE*>(file)));
#endif
}

bool FileSeek(FileHandle* file, s64 offset, FileSeekOrigin origin) {
    if (!file) {
        return false;
    }
    int seek_origin = SEEK_SET;
    switch (origin) {
    case FileSeekOrigin::Start:
        seek_origin = SEEK_SET;
        break;
    case FileSeekOrigin::Current:
        seek_origin = SEEK_CUR;
        break;
    case FileSeekOrigin::End:
        seek_origin = SEEK_END;
        break;
    }
#if defined(_WIN32)
    return _fseeki64(reinterpret_cast<FILE*>(file), offset, seek_origin) == 0;
#else
    return std::fseek(reinterpret_cast<FILE*>(file), offset, seek_origin) == 0;
#endif
}

void FileRewind(FileHandle* file) {
    if (!file) {
        return;
    }
    std::rewind(reinterpret_cast<FILE*>(file));
}

u64 FileRead(void* data, u64 size, u64 count, FileHandle* file) {
    if (!file || !data) {
        return 0;
    }
    return std::fread(data, static_cast<std::size_t>(size), static_cast<std::size_t>(count), reinterpret_cast<FILE*>(file));
}

bool FileFlush(FileHandle* file) {
    if (!file) {
        return false;
    }
    return std::fflush(reinterpret_cast<FILE*>(file)) == 0;
}

u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* file) {
    if (!file || !data) {
        return 0;
    }
    return std::fwrite(data, static_cast<std::size_t>(size), static_cast<std::size_t>(count), reinterpret_cast<FILE*>(file));
}

u64 FileWriteFormatted(FileHandle* file, const char* fmt, ...) {
    if (!file || !fmt) {
        return 0;
    }
    va_list args;
    va_start(args, fmt);
    const int written = std::vfprintf(reinterpret_cast<FILE*>(file), fmt, args);
    va_end(args);
    return written < 0 ? 0 : static_cast<u64>(written);
}

u64 FileLength(FileHandle* file) {
    if (!file) {
        return 0;
    }
    FILE* std_file = reinterpret_cast<FILE*>(file);
#if defined(_WIN32)
    const auto pos = _ftelli64(std_file);
    _fseeki64(std_file, 0, SEEK_END);
    const auto len = _ftelli64(std_file);
    _fseeki64(std_file, pos, SEEK_SET);
    return len < 0 ? 0 : static_cast<u64>(len);
#else
    const auto pos = std::ftell(std_file);
    std::fseek(std_file, 0, SEEK_END);
    const auto len = std::ftell(std_file);
    std::fseek(std_file, pos, SEEK_SET);
    return len < 0 ? 0 : static_cast<u64>(len);
#endif
}

void Log(LogLevel level, const char* fmt, ...) {
    if (!fmt) {
        return;
    }

    const char* level_name = "INFO";
    switch (level) {
    case LogLevel::Debug:
        level_name = "DEBUG";
        break;
    case LogLevel::Info:
        level_name = "INFO";
        break;
    case LogLevel::Warn:
        level_name = "WARN";
        break;
    case LogLevel::Error:
        level_name = "ERROR";
        break;
    }

    std::fprintf(stderr, "[melonDS:%s] ", level_name);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
}

Thread* Thread_Create(std::function<void()> func) {
    if (!func) {
        return nullptr;
    }
    auto* impl = new ThreadImpl(std::move(func));
    return reinterpret_cast<Thread*>(impl);
}

void Thread_Free(Thread* thread) {
    if (!thread) {
        return;
    }
    auto* impl = reinterpret_cast<ThreadImpl*>(thread);
    if (impl->thread.joinable()) {
        impl->thread.detach();
    }
    delete impl;
}

void Thread_Wait(Thread* thread) {
    if (!thread) {
        return;
    }
    auto* impl = reinterpret_cast<ThreadImpl*>(thread);
    if (impl->thread.joinable()) {
        impl->thread.join();
    }
}

Semaphore* Semaphore_Create() {
    return reinterpret_cast<Semaphore*>(new SemaphoreImpl());
}

void Semaphore_Free(Semaphore* sema) {
    delete reinterpret_cast<SemaphoreImpl*>(sema);
}

void Semaphore_Reset(Semaphore* sema) {
    if (!sema) {
        return;
    }
    auto* impl = reinterpret_cast<SemaphoreImpl*>(sema);
    std::scoped_lock lock(impl->mutex);
    impl->count = 0;
}

void Semaphore_Wait(Semaphore* sema) {
    if (!sema) {
        return;
    }
    auto* impl = reinterpret_cast<SemaphoreImpl*>(sema);
    std::unique_lock lock(impl->mutex);
    impl->condition.wait(lock, [&]() { return impl->count > 0; });
    --impl->count;
}

bool Semaphore_TryWait(Semaphore* sema, int timeout_ms) {
    if (!sema) {
        return false;
    }
    auto* impl = reinterpret_cast<SemaphoreImpl*>(sema);
    std::unique_lock lock(impl->mutex);

    if (timeout_ms <= 0) {
        if (impl->count <= 0) {
            return false;
        }
        --impl->count;
        return true;
    }

    const bool acquired = impl->condition.wait_for(
        lock, std::chrono::milliseconds(timeout_ms), [&]() { return impl->count > 0; });
    if (!acquired) {
        return false;
    }
    --impl->count;
    return true;
}

void Semaphore_Post(Semaphore* sema, int count) {
    if (!sema || count <= 0) {
        return;
    }
    auto* impl = reinterpret_cast<SemaphoreImpl*>(sema);
    {
        std::scoped_lock lock(impl->mutex);
        impl->count += count;
    }
    impl->condition.notify_all();
}

Mutex* Mutex_Create() {
    return reinterpret_cast<Mutex*>(new MutexImpl());
}

void Mutex_Free(Mutex* mutex) {
    delete reinterpret_cast<MutexImpl*>(mutex);
}

void Mutex_Lock(Mutex* mutex) {
    if (!mutex) {
        return;
    }
    reinterpret_cast<MutexImpl*>(mutex)->mutex.lock();
}

void Mutex_Unlock(Mutex* mutex) {
    if (!mutex) {
        return;
    }
    reinterpret_cast<MutexImpl*>(mutex)->mutex.unlock();
}

bool Mutex_TryLock(Mutex* mutex) {
    if (!mutex) {
        return false;
    }
    return reinterpret_cast<MutexImpl*>(mutex)->mutex.try_lock();
}

void Sleep(u64 usecs) {
    std::this_thread::sleep_for(std::chrono::microseconds(usecs));
}

u64 GetMSCount() {
    const auto elapsed = std::chrono::steady_clock::now() - kStart;
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

u64 GetUSCount() {
    const auto elapsed = std::chrono::steady_clock::now() - kStart;
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
}

void WriteNDSSave(const u8* savedata, u32 savelen, u32 writeoffset, u32 writelen, void* userdata) {
    (void)writeoffset;
    (void)writelen;
    if (auto* events = AsEvents(userdata)) {
        events->OnWriteNdsSave(savedata, savelen);
    }
}

void WriteGBASave(const u8* savedata, u32 savelen, u32 writeoffset, u32 writelen, void* userdata) {
    (void)writeoffset;
    (void)writelen;
    if (auto* events = AsEvents(userdata)) {
        events->OnWriteGbaSave(savedata, savelen);
    }
}

void WriteFirmware(const Firmware& firmware, u32 writeoffset, u32 writelen, void* userdata) {
    (void)writeoffset;
    (void)writelen;
    if (auto* events = AsEvents(userdata)) {
        events->OnWriteFirmware(firmware.Buffer(), firmware.Length());
    }
}

void WriteDateTime(int year, int month, int day, int hour, int minute, int second, void* userdata) {
    (void)year;
    (void)month;
    (void)day;
    (void)hour;
    (void)minute;
    (void)second;
    (void)userdata;
}

void MP_Begin(void* userdata) { (void)userdata; }
void MP_End(void* userdata) { (void)userdata; }
int MP_SendPacket(u8* data, int len, u64 timestamp, void* userdata) {
    (void)data;
    (void)len;
    (void)timestamp;
    (void)userdata;
    return 0;
}
int MP_RecvPacket(u8* data, u64* timestamp, void* userdata) {
    (void)data;
    (void)timestamp;
    (void)userdata;
    return 0;
}
int MP_SendCmd(u8* data, int len, u64 timestamp, void* userdata) {
    (void)data;
    (void)len;
    (void)timestamp;
    (void)userdata;
    return 0;
}
int MP_SendReply(u8* data, int len, u64 timestamp, u16 aid, void* userdata) {
    (void)data;
    (void)len;
    (void)timestamp;
    (void)aid;
    (void)userdata;
    return 0;
}
int MP_SendAck(u8* data, int len, u64 timestamp, void* userdata) {
    (void)data;
    (void)len;
    (void)timestamp;
    (void)userdata;
    return 0;
}
int MP_RecvHostPacket(u8* data, u64* timestamp, void* userdata) {
    (void)data;
    (void)timestamp;
    (void)userdata;
    return 0;
}
u16 MP_RecvReplies(u8* data, u64 timestamp, u16 aidmask, void* userdata) {
    (void)data;
    (void)timestamp;
    (void)aidmask;
    (void)userdata;
    return 0;
}

int Net_SendPacket(u8* data, int len, void* userdata) {
    (void)data;
    (void)len;
    (void)userdata;
    return 0;
}

int Net_RecvPacket(u8* data, void* userdata) {
    (void)data;
    (void)userdata;
    return 0;
}

void Camera_Start(int num, void* userdata) {
    (void)num;
    (void)userdata;
}
void Camera_Stop(int num, void* userdata) {
    (void)num;
    (void)userdata;
}
void Camera_CaptureFrame(int num, u32* frame, int width, int height, bool yuv, void* userdata) {
    (void)num;
    (void)frame;
    (void)width;
    (void)height;
    (void)yuv;
    (void)userdata;
}

void Mic_Start(void* userdata) { (void)userdata; }
void Mic_Stop(void* userdata) { (void)userdata; }
int Mic_ReadInput(s16* data, int maxlength, void* userdata) {
    (void)data;
    (void)maxlength;
    (void)userdata;
    return 0;
}

AACDecoder* AAC_Init() {
    return new AACDecoder();
}

void AAC_DeInit(AACDecoder* dec) {
    delete dec;
}

bool AAC_Configure(AACDecoder* dec, int frequency, int channels) {
    (void)dec;
    (void)frequency;
    (void)channels;
    return false;
}

bool AAC_DecodeFrame(AACDecoder* dec, const void* input, int inputlen, void* output, int outputlen) {
    (void)dec;
    (void)input;
    (void)inputlen;
    (void)output;
    (void)outputlen;
    return false;
}

bool Addon_KeyDown(KeyType type, void* userdata) {
    (void)type;
    (void)userdata;
    return false;
}

void Addon_RumbleStart(u32 len, void* userdata) {
    (void)len;
    (void)userdata;
}

void Addon_RumbleStop(void* userdata) {
    (void)userdata;
}

float Addon_MotionQuery(MotionQueryType type, void* userdata) {
    (void)type;
    (void)userdata;
    return 0.0f;
}

DynamicLibrary* DynamicLibrary_Load(const char* lib) {
    if (!lib) {
        return nullptr;
    }
    auto* out = new DynamicLibrary();
#if defined(_WIN32)
    out->handle = LoadLibraryA(lib);
#else
    out->handle = dlopen(lib, RTLD_NOW);
#endif
    if (!out->handle) {
        delete out;
        return nullptr;
    }
    return out;
}

void DynamicLibrary_Unload(DynamicLibrary* lib) {
    if (!lib) {
        return;
    }
#if defined(_WIN32)
    FreeLibrary(lib->handle);
#else
    dlclose(lib->handle);
#endif
    delete lib;
}

void* DynamicLibrary_LoadFunction(DynamicLibrary* lib, const char* name) {
    if (!lib || !name) {
        return nullptr;
    }
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(lib->handle, name));
#else
    return dlsym(lib->handle, name);
#endif
}

} // namespace melonDS::Platform
#endif
