#pragma once

#include <sdkconfig.h>
#include <memory>
#include <vector>
#if CONFIG_IDF_TARGET_ESP32S31
#include <esp_heap_caps.h>
#include <cstdlib>
#include <limits>
#include <new>
#endif

namespace app_memory {
#if CONFIG_IDF_TARGET_ESP32S31
// Non-DMA application metadata must not fall back to scarce internal SRAM.
// RTOS objects, driver DMA buffers and explicitly internal allocations keep
// using the IDF allocator and its internal-only reserve.
template<class T> class Allocator {
public:
    using value_type = T;
    using is_always_equal = std::true_type;
    Allocator() noexcept = default;
    template<class U> Allocator(const Allocator<U>&) noexcept {}
    T* allocate(size_t count) {
        void* p = nullptr;
        if (count <= std::numeric_limits<size_t>::max() / sizeof(T))
            p = heap_caps_malloc(count * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p && count) {
#if defined(__cpp_exceptions)
            throw std::bad_alloc();
#else
            std::abort(); // Same allocation-failure contract as operator new.
#endif
        }
        return static_cast<T*>(p);
    }
    void deallocate(T* p, size_t) noexcept { heap_caps_free(p); }
};
template<class T, class U>
bool operator==(const Allocator<T>&, const Allocator<U>&) noexcept { return true; }
template<class T, class U>
bool operator!=(const Allocator<T>&, const Allocator<U>&) noexcept { return false; }
#else
template<class T> using Allocator = std::allocator<T>;
#endif
template<class T> using Vector = std::vector<T, Allocator<T>>;
} // namespace app_memory
