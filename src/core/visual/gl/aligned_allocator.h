#pragma once
#include <cstddef>
#include <cstdlib>
#include <malloc.h>
#include <new>

template <typename T, std::size_t Alignment = 16>
class aligned_allocator {
public:
    typedef T value_type;
    typedef T* pointer;
    typedef const T* const_pointer;
    typedef T& reference;
    typedef const T& const_reference;
    typedef std::size_t size_type;
    typedef std::ptrdiff_t difference_type;

    template <typename U>
    struct rebind {
        typedef aligned_allocator<U, Alignment> other;
    };

    aligned_allocator() noexcept {}
    template <typename U>
    aligned_allocator(const aligned_allocator<U, Alignment>&) noexcept {}

    pointer allocate(size_type n) {
        if (n == 0) return nullptr;
        void* ptr = nullptr;
        if (Alignment <= 8) {
            ptr = malloc(n * sizeof(T));
        } else {
            ptr = memalign(Alignment, n * sizeof(T));
        }
        if (!ptr) throw std::bad_alloc();
        return static_cast<pointer>(ptr);
    }

    void deallocate(pointer p, size_type) noexcept {
        free(p);
    }
};

template <typename T, std::size_t A1, typename U, std::size_t A2>
inline bool operator==(const aligned_allocator<T, A1>&, const aligned_allocator<U, A2>&) noexcept {
    return A1 == A2;
}

template <typename T, std::size_t A1, typename U, std::size_t A2>
inline bool operator!=(const aligned_allocator<T, A1>&, const aligned_allocator<U, A2>&) noexcept {
    return A1 != A2;
}
