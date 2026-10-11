#include "activities/RecentBookCapacity.h"
#include <cassert>
#include <limits>
#include <vector>

namespace c = recent_book_capacity;
static_assert(c::next(0,1024)==32);
static_assert(c::next(32,1024)==64);
static_assert(c::next(64,1024)==128);
static_assert(c::next(128,1024)==256);
static_assert(c::next(64,100)==100);
static_assert(c::next(100,100)==0);
constexpr auto maximum=std::numeric_limits<std::size_t>::max();
static_assert(c::next(maximum/2+1,maximum)==maximum);
static_assert(c::next(maximum,maximum)==0);

int main() {
    std::vector<int> books;
    assert(c::reserve_one(books));
    assert(books.capacity()==32);
    for(int i=0;i<257;++i) {
        assert(c::reserve_one(books));
        books.push_back(i);
        const std::size_t expected=i<32?32:i<64?64:i<128?128:i<256?256:512;
        assert(books.capacity()==expected);
        for(int j=0;j<=i;++j) assert(books[j]==j);
    }
    books.clear();
    assert(c::reserve_one(books) && books.capacity()==512);
}
