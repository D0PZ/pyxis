#include "render/Snapshot.hpp"

#include "core/Error.hpp"
#include "core/Log.hpp"
#include "core/Text.hpp"

#include <knownfolders.h>
#include <shlobj.h>
#include <wincodec.h>

#include <array>
#include <cstdio>
#include <filesystem>

namespace pyxis {
namespace {

// SHGetKnownFolderPath reserva con CoTaskMemAlloc.
struct CoTaskMemDeleter {
    void operator()(void* p) const noexcept { ::CoTaskMemFree(p); }
};

[[nodiscard]] std::wstring PicturesFolder() {
    PWSTR raw = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &raw))) {
        return {};
    }
    const std::unique_ptr<wchar_t, CoTaskMemDeleter> owned(raw);
    return std::wstring(owned.get());
}

// Quita los caracteres que Windows no admite en un nombre de archivo. El titulo
// viene del nombre del medio, que ya es un nombre valido, pero una URL no.
[[nodiscard]] std::wstring SanitizeName(std::wstring_view name) {
    static constexpr std::wstring_view kForbidden = L"\\/:*?\"<>|";

    std::wstring clean;
    clean.reserve(name.size());
    for (const wchar_t character : name) {
        clean.push_back(kForbidden.find(character) == std::wstring_view::npos &&
                                character >= 0x20
                            ? character
                            : L'_');
    }
    while (!clean.empty() && (clean.back() == L' ' || clean.back() == L'.')) {
        clean.pop_back();
    }
    return clean.empty() ? std::wstring(L"captura") : clean;
}

}  // namespace

std::wstring BuildSnapshotPath(const std::wstring& mediaTitle, Micros position) {
    std::filesystem::path folder = PicturesFolder();
    if (folder.empty()) folder = std::filesystem::current_path();
    folder /= L"Pyxis";

    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) {
        // Si no se puede crear la carpeta se cae al directorio actual antes que
        // renunciar a la captura.
        PYXIS_WARN("no se pudo crear '{}': {}", folder.string(), error.message());
        folder = std::filesystem::current_path();
    }

    // Se recorta la extension del medio para no acabar con "pelicula.mkv_...png".
    std::wstring stem = std::filesystem::path(mediaTitle).stem().wstring();
    if (stem.empty()) stem = L"captura";

    const Micros clamped = position > 0 ? position : 0;
    const long long totalMs = clamped / 1000;

    std::array<wchar_t, 64> stamp{};
    std::swprintf(stamp.data(), stamp.size(), L"_%02lld-%02lld-%02lld.%03lld.png",
                  totalMs / 3600000, (totalMs / 60000) % 60, (totalMs / 1000) % 60,
                  totalMs % 1000);

    folder /= SanitizeName(stem) + stamp.data();
    return folder.wstring();
}

void SaveTextureAsPng(Device& device, ID3D11Texture2D* texture,
                      const std::wstring& path) {
    PYXIS_REQUIRE(texture != nullptr, "captura sin textura de origen");

    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);

    // La GPU no deja leer directamente un destino de render: hay que copiarlo a
    // una textura de preparacion accesible desde la CPU. Es la unica copia de
    // toda la ruta, y ocurre solo al pulsar la captura.
    D3D11_TEXTURE2D_DESC stagingDesc = description;
    stagingDesc.Usage          = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags      = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags      = 0;

    ComPtr<ID3D11Texture2D> staging;
    PYXIS_CHECK_HR(device.Handle()->CreateTexture2D(&stagingDesc, nullptr, &staging),
                   "no se pudo crear la textura de lectura para la captura");

    device.Context()->CopyResource(staging.Get(), texture);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    PYXIS_CHECK_HR(device.Context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                   "no se pudo leer la textura de la captura");

    // Desde aqui hay que desmapear pase lo que pase.
    struct Unmapper {
        ID3D11DeviceContext* context;
        ID3D11Texture2D*     resource;
        ~Unmapper() { context->Unmap(resource, 0); }
    } unmapper{device.Context(), staging.Get()};

    ComPtr<IWICImagingFactory> factory;
    PYXIS_CHECK_HR(::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                      CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
                   "no se pudo crear la fabrica de imagenes de Windows");

    ComPtr<IWICStream> stream;
    PYXIS_CHECK_HR(factory->CreateStream(&stream), "no se pudo crear el flujo de salida");
    PYXIS_CHECK_HR(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE),
                   "no se pudo crear el archivo de la captura");

    ComPtr<IWICBitmapEncoder> encoder;
    PYXIS_CHECK_HR(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder),
                   "no se pudo crear el codificador PNG");
    PYXIS_CHECK_HR(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache),
                   "no se pudo inicializar el codificador PNG");

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2>         options;
    PYXIS_CHECK_HR(encoder->CreateNewFrame(&frame, &options),
                   "no se pudo crear el fotograma del PNG");
    PYXIS_CHECK_HR(frame->Initialize(options.Get()),
                   "no se pudo inicializar el fotograma del PNG");

    PYXIS_CHECK_HR(frame->SetSize(description.Width, description.Height),
                   "no se pudo fijar el tamano del PNG");

    // Se pide BGRA sin alfa util: el video es opaco, y un PNG con canal alfa
    // ocuparia un tercio mas para no aportar nada.
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    PYXIS_CHECK_HR(frame->SetPixelFormat(&format),
                   "no se pudo fijar el formato del PNG");

    // WIC convierte de BGRA a BGR por su cuenta si se le entrega un origen con
    // el formato declarado, asi que se envuelve la memoria mapeada.
    ComPtr<IWICBitmap> source;
    PYXIS_CHECK_HR(factory->CreateBitmapFromMemory(
                       description.Width, description.Height,
                       GUID_WICPixelFormat32bppBGRA, mapped.RowPitch,
                       mapped.RowPitch * description.Height,
                       static_cast<BYTE*>(mapped.pData), &source),
                   "no se pudo envolver la imagen capturada");

    PYXIS_CHECK_HR(frame->WriteSource(source.Get(), nullptr),
                   "no se pudieron escribir los pixeles del PNG");
    PYXIS_CHECK_HR(frame->Commit(), "no se pudo cerrar el fotograma del PNG");
    PYXIS_CHECK_HR(encoder->Commit(), "no se pudo cerrar el PNG");

    PYXIS_INFO("Captura guardada en '{}' ({}x{})", ToUtf8(path),
               description.Width, description.Height);
}

}  // namespace pyxis
