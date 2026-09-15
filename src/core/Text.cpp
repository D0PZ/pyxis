#include "core/Text.hpp"

#include <windows.h>

namespace pyxis {

std::string ToUtf8(std::wstring_view wide) {
    if (wide.empty()) return {};

    const int needed = ::WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};

    std::string utf8(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                          utf8.data(), needed, nullptr, nullptr);
    return utf8;
}

std::wstring ToUtf16(std::string_view utf8) {
    if (utf8.empty()) return {};

    const int needed = ::MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return {};

    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                          wide.data(), needed);
    return wide;
}

}  // namespace pyxis
