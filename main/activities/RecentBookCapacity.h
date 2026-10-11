#pragma once
#include <cstddef>

namespace recent_book_capacity {
// Zero denotes an unrepresentable allocation, not a shelf-count limit.
constexpr std::size_t next(std::size_t capacity, std::size_t maximum) {
    if (capacity >= maximum) return 0;
    if (capacity == 0) return maximum < 32 ? maximum : 32;
    return capacity > maximum / 2 ? maximum : capacity * 2;
}
template<class Container>
bool reserve_one(Container &books) {
    if (books.size() < books.capacity()) return true;
    const auto capacity = next(books.capacity(), books.max_size());
    if (!capacity) return false;
    books.reserve(capacity);
    return true;
}
}
