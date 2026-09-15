#include "core/Thread.hpp"
#include "core/Log.hpp"

#include <windows.h>
#include <avrt.h>

namespace pyxis {
namespace {

const wchar_t* TaskName(MmcssTask task) noexcept {
    switch (task) {
        case MmcssTask::Playback: return L"Playback";
        case MmcssTask::ProAudio: return L"Pro Audio";
    }
    return L"Playback";
}

}  // namespace

MmcssScope::MmcssScope(MmcssTask task) noexcept {
    DWORD taskIndex = 0;   // 0 = que MMCSS asigne el indice
    handle_ = ::AvSetMmThreadCharacteristicsW(TaskName(task), &taskIndex);

    if (handle_ == nullptr) {
        PYXIS_WARN("MMCSS rechazo el registro del hilo (error {}); "
                   "la reproduccion continua sin prioridad reservada",
                   ::GetLastError());
        return;
    }

    // Dentro de la clase MMCSS, la prioridad mas alta disponible. No es la
    // prioridad de tiempo real del sistema: MMCSS limita el uso al 80% de CPU
    // para que un hilo multimedia no pueda congelar la maquina.
    ::AvSetMmThreadPriority(handle_, AVRT_PRIORITY_HIGH);
}

MmcssScope::~MmcssScope() {
    if (handle_ != nullptr) {
        ::AvRevertMmThreadCharacteristics(handle_);
    }
}

void SetCurrentThreadName(const wchar_t* name) noexcept {
    // SetThreadDescription existe desde Windows 10 1607 y Pyxis exige
    // Windows 11, asi que puede llamarse directamente.
    ::SetThreadDescription(::GetCurrentThread(), name);
}

}  // namespace pyxis
