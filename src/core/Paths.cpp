#include "core/Paths.hpp"

#include "core/Log.hpp"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <memory>

namespace pyxis {
namespace {

// SHGetKnownFolderPath reserva con CoTaskMemAlloc.
struct CoTaskMemDeleter {
    void operator()(void* p) const noexcept { ::CoTaskMemFree(p); }
};

[[nodiscard]] std::wstring KnownFolder(const KNOWNFOLDERID& id) {
    PWSTR raw = nullptr;
    if (FAILED(::SHGetKnownFolderPath(id, 0, nullptr, &raw))) return {};

    const std::unique_ptr<wchar_t, CoTaskMemDeleter> owned(raw);
    return std::wstring(owned.get());
}

}  // namespace

std::filesystem::path PyxisOutputFolder(UserFolder folder) {
    const KNOWNFOLDERID& id =
        folder == UserFolder::Videos ? FOLDERID_Videos : FOLDERID_Pictures;

    std::filesystem::path root = KnownFolder(id);
    if (root.empty()) root = std::filesystem::current_path();
    root /= L"Pyxis";

    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error) {
        PYXIS_WARN("no se pudo crear '{}': {}", root.string(), error.message());
        return std::filesystem::current_path();
    }
    return root;
}

std::wstring SanitizeFileName(std::wstring_view name) {
    static constexpr std::wstring_view kForbidden = L"\\/:*?\"<>|";

    std::wstring clean;
    clean.reserve(name.size());
    for (const wchar_t character : name) {
        clean.push_back(kForbidden.find(character) == std::wstring_view::npos &&
                                character >= 0x20
                            ? character
                            : L'_');
    }
    // Windows no admite que un nombre acabe en espacio o punto.
    while (!clean.empty() && (clean.back() == L' ' || clean.back() == L'.')) {
        clean.pop_back();
    }
    return clean.empty() ? std::wstring(L"pyxis") : clean;
}

}  // namespace pyxis
