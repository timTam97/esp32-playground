#pragma once
#include <stddef.h>
#include <stdint.h>

namespace jpeg_wire {
constexpr size_t header_size = 32;
constexpr size_t chunk_size = 10000;
constexpr size_t maximum_frame = 512 * 1024;
constexpr uint32_t magic = 0x474a5047; // GJPG

inline void u16(uint8_t* p, uint16_t value) {
    p[0] = value >> 8; p[1] = value;
}
inline void u32(uint8_t* p, uint32_t value) {
    p[0] = value >> 24; p[1] = value >> 16; p[2] = value >> 8; p[3] = value;
}
inline void header(uint8_t* p, uint32_t frame_id, uint64_t captured_ms,
                   uint32_t total, uint16_t index, uint16_t payload) {
    u32(p, magic);
    p[4] = 1;
    p[5] = 0;
    u16(p + 6, header_size);
    u32(p + 8, frame_id);
    u32(p + 12, captured_ms >> 32);
    u32(p + 16, captured_ms);
    u32(p + 20, total);
    u16(p + 24, index);
    u16(p + 26, (total + chunk_size - 1) / chunk_size);
    u16(p + 28, payload);
    u16(p + 30, 0);
}
}
