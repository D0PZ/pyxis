#include "core/Error.hpp"

#include <windows.h>

extern "C" {
#include <libavutil/error.h>
}

#include <array>
#include <cstdio>

namespace pyxis {
namespace {

const char* DomainName(ErrorDomain domain) noexcept {
    switch (domain) {
        case ErrorDomain::Logic:   return "Logica";
        case ErrorDomain::Win32:   return "Win32";
        case ErrorDomain::HResult: return "HRESULT";
        case ErrorDomain::FFmpeg:  return "FFmpeg";
    }
    return "Desconocido";
}

// FormatMessage devuelve UTF-16; el resto del proyecto trabaja en UTF-8.
std::string FormatSystemMessage(unsigned long code) {
    wchar_t* buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);

    if (length == 0 || buffer == nullptr) {
        if (buffer) ::LocalFree(buffer);
        return "sin descripcion del sistema";
    }

    // Recorta el CRLF final que anade FormatMessage.
    std::wstring_view wide(buffer, length);
    while (!wide.empty() && (wide.back() == L'\r' || wide.back() == L'\n')) {
        wide.remove_suffix(1);
    }

    std::string utf8;
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed > 0) {
        utf8.resize(static_cast<std::size_t>(needed));
        ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                              utf8.data(), needed, nullptr, nullptr);
    }
    ::LocalFree(buffer);
    return utf8;
}

std::string ToHex(std::int64_t value) {
    std::array<char, 32> buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(), "0x%08llX",
                                      static_cast<unsigned long long>(
                                          static_cast<std::uint32_t>(value)));
    return std::string(buffer.data(), written > 0 ? static_cast<std::size_t>(written) : 0);
}

}  // namespace

std::string DescribeError(ErrorDomain domain, std::int64_t code) {
    switch (domain) {
        case ErrorDomain::HResult:
            return ToHex(code) + " (" + FormatSystemMessage(
                       static_cast<unsigned long>(static_cast<std::uint32_t>(code))) + ")";

        case ErrorDomain::Win32:
            return std::to_string(code) + " (" + FormatSystemMessage(
                       static_cast<unsigned long>(code)) + ")";

        case ErrorDomain::FFmpeg: {
            std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
            ::av_strerror(static_cast<int>(code), buffer.data(), buffer.size());
            return std::to_string(code) + " (" + buffer.data() + ")";
        }

        case ErrorDomain::Logic:
            return "invariante interna";
    }
    return "codigo desconocido";
}

Exception::Exception(ErrorDomain domain, std::int64_t code, std::string context)
    : domain_(domain), code_(code), context_(std::move(context)) {
    message_ = context_;
    if (domain_ != ErrorDomain::Logic) {
        message_ += " [";
        message_ += DomainName(domain_);
        message_ += ' ';
        message_ += DescribeError(domain_, code_);
        message_ += ']';
    }
}

void ThrowHResult(long hr, std::string_view context) {
    throw Exception(ErrorDomain::HResult, hr, std::string(context));
}

void ThrowWin32(unsigned long err, std::string_view context) {
    throw Exception(ErrorDomain::Win32, static_cast<std::int64_t>(err), std::string(context));
}

void ThrowAv(int averr, std::string_view context) {
    throw Exception(ErrorDomain::FFmpeg, averr, std::string(context));
}

void ThrowLogic(std::string_view context) {
    throw Exception(ErrorDomain::Logic, 0, std::string(context));
}

}  // namespace pyxis
