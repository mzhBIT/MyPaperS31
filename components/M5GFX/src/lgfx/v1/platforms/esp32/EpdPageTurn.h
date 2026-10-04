#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lgfx_epd_page_turn {
constexpr int bands = 4;
constexpr int max_bands = 8;
constexpr int stagger = 3;
struct Rect { uint16_t x = 0, y = 0, w = 0, h = 0; };
constexpr int fast_spread=(bands-1)*stagger;
constexpr int slow_spread=20;
constexpr int band_delay(int band,int spread=fast_spread,int band_count=bands) {
    return (band*spread+(band_count==8 ? (band_count-1)/2 : 0))/(band_count-1);
}
constexpr int phases(int waveform_phases,int spread=fast_spread) { return waveform_phases + spread; }
constexpr int local_phase(int phase, int logical_band, int direction)
{
    return phase - (direction > 0 ? bands-1-logical_band : logical_band)*stagger;
}

inline bool valid_direction(int direction) { return direction && direction>=-4 && direction<=4; }
template<typename Visitor>
inline void for_each_dependency(int phase,int waveform_phases,const uint16_t *map,Visitor visit,int spread=fast_spread,int band_count=bands) {
    for(int band=0;band<band_count;++band) {
        const int local=phase-band_delay(band,spread,band_count);
        if(local>=0 && local<waveform_phases) visit(map ? int(map[local]) : local);
    }
}
inline const char *direction_name(int direction) {
    switch (direction) {
    case 1: return "right-to-left"; case -1: return "left-to-right";
    case 2: return "bottom-to-top"; case -2: return "top-to-bottom";
    case 3: return "bottom-right-to-top-left"; case -3: return "top-left-to-bottom-right";
    case 4: return "bottom-left-to-top-right"; case -4: return "top-right-to-bottom-left";
    default: return "off";
    }
}

// One band per physical 16-pixel bus group. Never split or rearrange packed
// drive bytes. Projection in logical screen coordinates supports all rotations.
inline int projected_band(int px,int py,int width,int height,int rotation,int direction,int band_count=bands) {
    int x=px,y=py;
    if ((1<<rotation)&0b11000110) x=width-1-x;
    if ((1<<rotation)&0b10011100) y=height-1-y;
    if (rotation&1) { const int t=x; x=y; y=t; }
    const int w=(rotation&1)?height:width, h=(rotation&1)?width:height;
    int position=0,extent=1;
    switch (direction) {
    case 1: position=w-1-x; extent=w; break;
    case -1: position=x; extent=w; break;
    case 2: position=h-1-y; extent=h; break;
    case -2: position=y; extent=h; break;
    case 3: position=w-1-x+h-1-y; extent=w+h-1; break;
    case -3: position=x+y; extent=w+h-1; break;
    case 4: position=x+h-1-y; extent=w+h-1; break;
    case -4: position=w-1-x+y; extent=w+h-1; break;
    }
    return position*band_count/extent;
}
inline void make_band_map(uint8_t *out,int width,int height,int rotation,int direction,int band_count=bands) {
    for (int y=0;y<height;++y)
        for (int group=0;group<width/16;++group)
            out[size_t(y)*(width/16)+group]=uint8_t(projected_band(group*16+8,y,width,height,rotation,direction,band_count));
}
inline void compose_projected(uint8_t *out,size_t frame_bytes,size_t width,
                             const uint8_t *base,const uint16_t *map,
                             int waveform_phases,int phase,const uint8_t *band_map,int spread=fast_spread,int band_count=bands) {
    std::memset(out,0,frame_bytes);
    const size_t groups=width/16,rows=frame_bytes/(width/4);
    for (size_t y=0;y<rows;++y) {
        size_t first=0;
        while (first<groups) {
            const int band=band_map[y*groups+first];
            size_t end=first+1;
            while (end<groups && band_map[y*groups+end]==band) ++end;
            const int local=phase-band_delay(band,spread,band_count);
            if (local>=0 && local<waveform_phases) {
                const size_t offset=(y*groups+first)*4;
                const uint8_t *src=base+size_t(map?map[local]:local)*frame_bytes;
                std::memcpy(out+offset,src+offset,(end-first)*4);
            }
            first=end;
        }
    }
}

struct Span { uint16_t first=0, count=0; }; // Whole 16-pixel groups.
enum class LayoutKind { RowBlocks, ColumnBlocks, DiagonalRows };
struct Layout {
    int width=0,height=0,rotation=0,direction=0;
    int band_count=bands;
    LayoutKind kind=LayoutKind::RowBlocks;
    Rect rects[max_bands]{};
    Span *rows=nullptr; // PSRAM, height * band_count; diagonal directions only.
    bool matches(int w,int h,int r,int d,int count=bands) const {
        return width==w && height==h && rotation==r && direction==d && band_count==count;
    }
};
inline bool prepare_layout(Layout &layout,int width,int height,int rotation,int direction,Span *rows,int band_count=bands) {
    if (!valid_direction(direction) || rotation<0 || rotation>=8 || width<=0 || width%16 || height<=0) return false;
    if (band_count<2 || band_count>max_bands) return false;
    Layout next;
    next.width=width; next.height=height; next.rotation=rotation; next.direction=direction;
    next.band_count=band_count;
    if (direction>=-2 && direction<=2) {
        const bool full_rows=projected_band(8,0,width,height,rotation,direction,band_count)==
                             projected_band(width-8,0,width,height,rotation,direction,band_count);
        next.kind=full_rows ? LayoutKind::RowBlocks : LayoutKind::ColumnBlocks;
        const int length=full_rows ? height : width/16;
        int first=0;
        while (first<length) {
            const int band=projected_band(full_rows?8:first*16+8,full_rows?first:0,width,height,rotation,direction,band_count);
            int end=first+1;
            while (end<length && projected_band(full_rows?8:end*16+8,full_rows?end:0,width,height,rotation,direction,band_count)==band) ++end;
            next.rects[band]=full_rows ? Rect{0,uint16_t(first),uint16_t(width),uint16_t(end-first)}
                                     : Rect{uint16_t(first*16),0,uint16_t((end-first)*16),uint16_t(height)};
            first=end;
        }
    } else {
        if (!rows) return false;
        next.kind=LayoutKind::DiagonalRows; next.rows=rows;
        for (int y=0;y<height;++y) {
            for (int band=0;band<band_count;++band) rows[size_t(y)*band_count+band]=Span{};
            for (int group=0;group<width/16;++group) {
                const int band=projected_band(group*16+8,y,width,height,rotation,direction,band_count);
                auto &span=rows[size_t(y)*band_count+band];
                if (!span.count) span.first=uint16_t(group);
                ++span.count;
            }
        }
    }
    layout=next;
    return true;
}
inline void write_span(uint8_t *out,const uint8_t *src,size_t offset,size_t bytes) {
    if (src) std::memcpy(out+offset,src+offset,bytes);
    else std::memset(out+offset,0,bytes); // Neutral drive, not a white pixel.
}
// Two disjoint passes: copy active bands, zero only inactive bands. Together
// they write every output byte exactly once, even in a reused/dirty buffer.
// Separate passes retain clear/copy timings without timers in row loops.
inline void copy_layout(uint8_t *out,size_t frame_bytes,const uint8_t *base,const uint16_t *map,
                        int waveform_phases,int phase,const Layout &layout,bool neutral_only=false,int spread=fast_spread) {
    const size_t stride=size_t(layout.width)/4;
    for (int band=0;band<layout.band_count;++band) {
        const int local=phase-band_delay(band,spread,layout.band_count);
        const bool active=local>=0 && local<waveform_phases;
        if (active==neutral_only) continue;
        const uint8_t *src=active ? base+size_t(map?map[local]:local)*frame_bytes : nullptr;
        if (layout.kind==LayoutKind::RowBlocks) {
            const auto &r=layout.rects[band];
            if (r.h) write_span(out,src,size_t(r.y)*stride,size_t(r.h)*stride);
        } else if (layout.kind==LayoutKind::ColumnBlocks) {
            const auto &r=layout.rects[band];
            if (!r.w) continue;
            for (int y=0;y<layout.height;++y) {
                const size_t offset=size_t(y)*stride+r.x/4;
                write_span(out,src,offset,r.w/4);
            }
        } else {
            for (int y=0;y<layout.height;++y) {
                const auto &span=layout.rows[size_t(y)*layout.band_count+band];
                if (!span.count) continue;
                const size_t offset=size_t(y)*stride+size_t(span.first)*4;
                write_span(out,src,offset,size_t(span.count)*4);
            }
        }
    }
}
inline void clear_inactive_layout(uint8_t *out,size_t frame_bytes,int waveform_phases,int phase,const Layout &layout,int spread=fast_spread) {
    copy_layout(out,frame_bytes,nullptr,nullptr,waveform_phases,phase,layout,true,spread);
}

// Copy only whole 16-pixel bus groups: their packed/permuted byte order stays intact.
inline bool valid_rects(const Rect *rects, size_t width, size_t height)
{
    size_t area = 0;
    for (int i=0; i<bands; ++i) {
        const auto &r = rects[i];
        if (!r.w || !r.h || r.x%16 || r.w%16 || r.x+r.w>width || r.y+r.h>height) return false;
        area += size_t(r.w)*r.h;
        for (int j=0; j<i; ++j) {
            const auto &s = rects[j];
            if (r.x<s.x+s.w && s.x<r.x+r.w && r.y<s.y+s.h && s.y<r.y+r.h) return false;
        }
    }
    return area == width*height;
}
inline void compose(uint8_t *out, size_t frame_bytes, size_t width,
                    const uint8_t *base, const uint16_t *map,
                    int waveform_phases, int phase, int direction, const Rect *rects)
{
    std::memset(out, 0, frame_bytes); // Neutral drive, not a white pixel.
    const size_t stride = width/4;
    for (int band=0; band<bands; ++band) {
        const int local = local_phase(phase, band, direction);
        if (local<0 || local>=waveform_phases) continue;
        const auto &r = rects[band];
        const uint8_t *src = base + size_t(map ? map[local] : local)*frame_bytes;
        if (r.x==0 && r.w==width) {
            std::memcpy(out+size_t(r.y)*stride, src+size_t(r.y)*stride, size_t(r.h)*stride);
        } else {
            for (size_t y=r.y; y<size_t(r.y)+r.h; ++y)
                std::memcpy(out+y*stride+r.x/4, src+y*stride+r.x/4, r.w/4);
        }
    }
}
}
