#include "core/Log.hpp"

#include "core/Text.hpp"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <mutex>

namespace pyxis::log {
namespace {

std::mutex g_mutex;
HANDLE     g_file = INVALID_HANDLE_VALUE;

const char* LevelTag(Level level) noexcept {
    switch (level) {
        case Level::Trace: return "TRZ";
        case Level::Debug: return "DBG";
        case Level::Info:  return "INF";
        case Level::Warn:  return "ADV";
        case Level::Error: return "ERR";
        case Level::Off:   return "---";
    }
    return "???";
}


}  // namespace

namespace detail {

void Emit(Level level, std::string_view message) noexcept {
    using namespace std::chrono;

    // Marca de tiempo monotona desde el arranque del proceso: para correlacionar
    // eventos de reproduccion interesa el delta, no la hora del reloj de pared.
    static const steady_clock::time_point start = steady_clock::now();
    const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - start).count();

    char line[1024];
    const int written = std::snprintf(
        line, sizeof(line), "[%8lld.%03lld] [%s] [%05lu] %.*s\r\n",
        static_cast<long long>(elapsed / 1000), static_cast<long long>(elapsed % 1000),
        LevelTag(level), ::GetCurrentThreadId(),
        static_cast<int>(message.size()), message.data());

    if (written <= 0) return;
    const std::size_t length =
        (static_cast<std::size_t>(written) < sizeof(line))
            ? static_cast<std::size_t>(written)
            : sizeof(line) - 1;

    ::OutputDebugStringW(ToUtf16(std::string_view(line, length)).c_str());

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file != INVALID_HANDLE_VALUE) {
        DWORD ignored = 0;
        ::WriteFile(g_file, line, static_cast<DWORD>(length), &ignored, nullptr);
    }
}

}  // namespace detail

void SetLevel(Level level) noexcept {
    detail::g_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

Level GetLevel() noexcept {
    return static_cast<Level>(detail::g_level.load(std::memory_order_relaxed));
}

void SetLogFile(const std::wstring& path) noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file != INVALID_HANDLE_VALUE) {
        ::CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    if (path.empty()) return;

    g_file = ::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void Shutdown() noexcept {
    SetLogFile({});
}

}  // namespace pyxis::log
