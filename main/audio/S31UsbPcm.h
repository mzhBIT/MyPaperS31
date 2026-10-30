#pragma once
#include <cstddef>
#include <cstdint>

namespace s31_usb_pcm {
inline bool valid_bits(unsigned bits) { return bits==16 || bits==24 || bits==32; }

// Read packed little-endian PCM without alignment assumptions. A 24-bit sample
// occupies exactly three bytes, matching the P4 packed-24 output path.
inline int32_t sample(const uint8_t *data, unsigned bits) {
    uint32_t value=0;
    for (unsigned b=0;b<bits/8;++b) value |= uint32_t(data[b])<<(8*b);
    const int64_t signed_value=(value & (uint32_t(1)<<(bits-1)))
        ? int64_t(value)-(int64_t(1)<<bits) : int64_t(value);
    return int32_t(signed_value);
}

// Input is interleaved stereo at the converter's actual bit depth. Keep both
// channels/low bits for USB; BH1098G's mono I2S slot layout does not apply.
inline size_t pack(uint8_t *out, const uint8_t *in, size_t frames,
                   unsigned input_bits, unsigned channels, unsigned bits, int gain_q16) {
    if ((channels != 1 && channels != 2) || !valid_bits(input_bits) || !valid_bits(bits)) return 0;
    const unsigned input_bytes=input_bits/8;
    size_t bytes = 0;
    for (size_t f=0;f<frames;++f) {
        for (unsigned c=0;c<channels;++c) {
            int64_t value=sample(in+(f*2+c)*input_bytes,input_bits);
            if (channels==1)
                value=(int64_t(sample(in+f*2*input_bytes,input_bits))+
                       sample(in+(f*2+1)*input_bytes,input_bits))/2;
            if (bits>input_bits) value *= int64_t(1)<<(bits-input_bits);
            else if (bits<input_bits) value /= int64_t(1)<<(input_bits-bits);
            value=value*gain_q16/65536;
            const uint32_t packed=uint32_t(value);
            for (unsigned b=0;b<bits/8;++b) out[bytes++] = uint8_t(packed>>(b*8));
        }
    }
    return bytes;
}
}
