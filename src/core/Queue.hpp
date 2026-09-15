// ============================================================================
//  Queue.hpp - Cola acotada y bloqueante entre etapas del pipeline
//
//  Por que NO es lock-free: las colas de este reproductor mueven entre 25 y
//  120 elementos por segundo. A esa frecuencia, el coste de un mutex sin
//  contencion (decenas de nanosegundos) es irrelevante frente a los
//  milisegundos que cuesta decodificar un fotograma 4K. Una cola lock-free
//  aportaria complejidad y modos de fallo sutiles a cambio de nada medible.
//  El unico camino donde si importa es el reloj maestro, y ese ya es atomico
//  sin cerrojos (ver Clock.hpp).
//
//  Lo que SI importa aqui es que la cola sea ACOTADA. Un archivo 8K puede
//  demultiplexarse mucho mas rapido de lo que se decodifica; sin un limite, el
//  demultiplexor se comeria toda la RAM en segundos. El limite convierte eso en
//  contrapresion: el productor duerme hasta que el consumidor avanza.
// ============================================================================
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace pyxis {

enum class QueueStatus {
    Ok,        // operacion completada
    Closed,    // la cola se cerro; no llegaran mas elementos
    Flushed,   // interrumpida por un vaciado (salto de posicion)
};

// El vaciado se senaliza con una EPOCA, no con una bandera persistente.
//
// Con una bandera hay que levantarla y volver a bajarla, y entre ambos momentos
// cada Pop devuelve Flushed de inmediato: los hilos de decodificacion girarian
// en vacio consumiendo CPU hasta que alguien la bajase. Con una epoca, cada
// operacion compara el valor que vio al entrar con el actual; quien estuviera
// esperando durante el vaciado se entera una unica vez, y quien llegue despues
// se bloquea con normalidad. De paso desaparece la necesidad de un "fin de
// vaciado", con lo que el protocolo tiene la mitad de estados.

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {}

    // Encola, esperando si esta llena. Devuelve Closed/Flushed si la espera se
    // interrumpe, y en ese caso `value` no se consume.
    QueueStatus Push(T&& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        const std::uint64_t epoch = flushEpoch_;

        notFull_.wait(lock, [this, epoch] {
            return items_.size() < capacity_ || closed_ || flushEpoch_ != epoch;
        });

        if (closed_)              return QueueStatus::Closed;
        if (flushEpoch_ != epoch) return QueueStatus::Flushed;

        items_.push_back(std::move(value));
        lock.unlock();
        notEmpty_.notify_one();
        return QueueStatus::Ok;
    }

    // Desencola, esperando si esta vacia. Una cola cerrada sigue entregando lo
    // que le quede pendiente antes de reportar Closed: al llegar al final del
    // archivo hay que reproducir la cola, no tirarla.
    QueueStatus Pop(T& out) {
        std::unique_lock<std::mutex> lock(mutex_);
        const std::uint64_t epoch = flushEpoch_;

        notEmpty_.wait(lock, [this, epoch] {
            return !items_.empty() || closed_ || flushEpoch_ != epoch;
        });

        if (flushEpoch_ != epoch) return QueueStatus::Flushed;
        if (items_.empty())       return QueueStatus::Closed;

        out = std::move(items_.front());
        items_.pop_front();
        lock.unlock();
        notFull_.notify_one();
        return QueueStatus::Ok;
    }

    // Desencola sin esperar. Lo usa el renderizador, que no puede bloquearse:
    // si no hay fotograma nuevo, repite el anterior y sigue.
    [[nodiscard]] bool TryPop(T& out) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (items_.empty()) return false;
        out = std::move(items_.front());
        items_.pop_front();
        lock.unlock();
        notFull_.notify_one();
        return true;
    }

    // Descarta el contenido y despierta a todos los que esperan. Se usa al
    // saltar de posicion: los datos en vuelo pertenecen a la posicion anterior
    // y mostrarlos produciria un parpadeo visible.
    // La cola queda operativa de inmediato: no hay que "cerrar" el vaciado.
    void Flush() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            items_.clear();
            ++flushEpoch_;
        }
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    // Cierra la cola definitivamente (fin del archivo o apagado).
    void Close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    // Vuelve a admitir elementos tras un Close. Se usa al abrir un medio nuevo.
    void Reopen() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = false;
        items_.clear();
        ++flushEpoch_;
    }

    [[nodiscard]] std::size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }

    [[nodiscard]] std::size_t Capacity() const noexcept { return capacity_; }

    [[nodiscard]] bool IsClosed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

private:
    mutable std::mutex      mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
    std::deque<T>           items_;
    const std::size_t       capacity_;
    std::uint64_t           flushEpoch_ = 0;
    bool                    closed_     = false;
};

}  // namespace pyxis
