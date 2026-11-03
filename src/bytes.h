// Little-endian byte writing and reading, for snapshots and (later) network messages.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

struct ByteWriter {
    std::vector<uint8_t>& out;
    explicit ByteWriter(std::vector<uint8_t>& o) : out(o) {}
    void u8(uint8_t v) { out.push_back(v); }
    void u16(uint16_t v) { u8((uint8_t)v); u8((uint8_t)(v >> 8)); }
    void u32(uint32_t v) { u16((uint16_t)v); u16((uint16_t)(v >> 16)); }
    void u64(uint64_t v) { u32((uint32_t)v); u32((uint32_t)(v >> 32)); }
    void i16(int16_t v) { u16((uint16_t)v); }
    void i32(int32_t v) { u32((uint32_t)v); }
    void f32(float v) { uint32_t b; memcpy(&b, &v, 4); u32(b); }
    void f64(double v) { uint64_t b; memcpy(&b, &v, 8); u64(b); }
};

// Reads past the end come back as zeros and set `bad`, so a short or corrupt message can be checked once at the end.
struct ByteReader {
    const uint8_t* p;
    size_t n, at = 0;
    bool bad = false;
    ByteReader(const uint8_t* data, size_t size) : p(data), n(size) {}
    size_t left() const { return n - at; }
    uint8_t u8() {
        if (at >= n) { bad = true; return 0; }
        return p[at++];
    }
    uint16_t u16() { uint16_t a = u8(); return (uint16_t)(a | (uint16_t)u8() << 8); }
    uint32_t u32() { uint32_t a = u16(); return a | (uint32_t)u16() << 16; }
    uint64_t u64() { uint64_t a = u32(); return a | (uint64_t)u32() << 32; }
    int16_t i16() { return (int16_t)u16(); }
    int32_t i32() { return (int32_t)u32(); }
    float f32() { uint32_t b = u32(); float v; memcpy(&v, &b, 4); return v; }
    double f64() { uint64_t b = u64(); double v; memcpy(&v, &b, 8); return v; }
};
