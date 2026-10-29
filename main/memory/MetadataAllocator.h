#pragma once

#include <sdkconfig.h>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <initializer_list>
#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#include <cstdlib>
#include <limits>
#include <new>
#endif

namespace metadata_memory {
struct Budget {
    static constexpr size_t max_entries=16384;
    static constexpr size_t max_bytes=2*1024*1024;
    size_t used=0;
    bool exceeded=false;
    bool add(size_t count,size_t record_bytes,std::initializer_list<size_t> strings) {
        size_t cost=record_bytes*3+128; // Includes vector growth/map/index overhead.
        for(size_t n:strings) {
            if(n>16384 || cost>max_bytes-2*n) {exceeded=true;return false;}
            cost+=2*n;
        }
        if(count>=max_entries || cost>max_bytes || used>max_bytes-cost) {exceeded=true;return false;}
#if defined(ESP_PLATFORM)
        if(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)<cost+512*1024) {exceeded=true;return false;}
#endif
        used+=cost;return true;
    }
    static bool reserve_allowed(size_t count,size_t record_bytes) {
        if(count>max_entries || record_bytes>max_bytes || count>max_bytes/record_bytes) return false;
#if defined(ESP_PLATFORM)
        return heap_caps_get_free_size(MALLOC_CAP_SPIRAM)>count*record_bytes+512*1024 &&
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)>=count*record_bytes;
#else
        return true;
#endif
    }
};
// Book metadata must never consume the driver's internal/DMA reserve.
template<class T> class Allocator {
public:
    using value_type=T;
    using is_always_equal=std::true_type;
    Allocator() noexcept=default;
    template<class U> Allocator(const Allocator<U>&) noexcept {}
    T* allocate(size_t count) {
#if defined(ESP_PLATFORM)
        void* p=nullptr;
        if(count<=std::numeric_limits<size_t>::max()/sizeof(T))
            p=heap_caps_malloc(count*sizeof(T),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!p && count) {
#if defined(__cpp_exceptions)
            throw std::bad_alloc();
#else
            std::abort();
#endif
        }
        return static_cast<T*>(p);
#else
        return std::allocator<T>{}.allocate(count);
#endif
    }
    void deallocate(T* p,size_t count) noexcept {
#if defined(ESP_PLATFORM)
        heap_caps_free(p);
#else
        std::allocator<T>{}.deallocate(p,count);
#endif
    }
};
template<class T,class U> bool operator==(Allocator<T>,Allocator<U>){return true;}
template<class T,class U> bool operator!=(Allocator<T>,Allocator<U>){return false;}

class String : public std::basic_string<char,std::char_traits<char>,Allocator<char>> {
    using Base=std::basic_string<char,std::char_traits<char>,Allocator<char>>;
public:
    using Base::Base;
    using Base::operator=;
    String()=default;
    String(const Base& s):Base(s){}
    String(const std::string& s):Base(s.data(),s.size()){}
    String(std::string_view s):Base(s.data(),s.size()){}
    operator std::string() const {return std::string(data(),size());}
    String substr(size_t pos=0,size_t count=Base::npos)const{return String(Base::substr(pos,count));}
    friend bool operator==(const String& a,const String& b){return std::string_view(a.data(),a.size())==std::string_view(b.data(),b.size());}
    friend bool operator==(const String& a,const std::string& b){return std::string_view(a.data(),a.size())==b;}
    friend bool operator==(const std::string& a,const String& b){return b==a;}
    friend bool operator!=(const String& a,const std::string& b){return !(a==b);}
    friend bool operator!=(const std::string& a,const String& b){return !(a==b);}
    friend bool operator==(const String& a,const char* b){return a.compare(b)==0;}
    friend bool operator==(const char* a,const String& b){return b==a;}
    friend bool operator!=(const String& a,const char* b){return !(a==b);}
    friend bool operator!=(const char* a,const String& b){return !(b==a);}
    friend std::string operator+(const String& a,const char* b){return std::string(a)+b;}
    friend std::string operator+(const char* a,const String& b){return a+std::string(b);}
    friend std::string operator+(const String& a,const std::string& b){return std::string(a)+b;}
    friend std::string operator+(const std::string& a,const String& b){return a+std::string(b);}
    friend std::string operator+(const String& a,const String& b){return std::string(a)+std::string(b);}
};
struct Hash {size_t operator()(const String& s)const noexcept{return std::hash<std::string_view>{}({s.data(),s.size()});}};
template<class T> using Vector=std::vector<T,Allocator<T>>;
template<class T> using Map=std::unordered_map<String,T,Hash,std::equal_to<String>,Allocator<std::pair<const String,T>>>;
template<class T> void release(T& value){T{}.swap(value);}
} // namespace metadata_memory
