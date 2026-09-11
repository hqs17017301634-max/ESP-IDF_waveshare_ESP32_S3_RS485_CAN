#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dns_wire {
constexpr size_t MaxMessage = 4096;
constexpr size_t MaxQuery = 1232;
inline uint16_t u16(const uint8_t *p) { return (uint16_t(p[0]) << 8) | p[1]; }
inline uint32_t u32(const uint8_t *p) { return (uint32_t(u16(p)) << 16) | u16(p+2); }
inline void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
inline void put32(uint8_t *p, uint32_t v) { put16(p,v>>16); put16(p+2,v); }
struct Question {
    char name[256]{};
    uint16_t type = 0;
    size_t end = 0;
};
inline bool question(const uint8_t *p, size_t n, Question &q) {
    if (n < 17 || u16(p+4) != 1 || (p[2] & 0x78)) return false;
    size_t pos = 12, out = 0;
    while (pos < n) {
        const uint8_t len = p[pos++];
        if (!len) {
            if (pos+4 > n || u16(p+pos+2) != 1) return false;
            if (!out) q.name[out++] = '.';
            q.name[out] = 0; q.type = u16(p+pos); q.end = pos+4;
            return true;
        }
        if (len > 63 || pos+len > n || out+len+1 >= sizeof(q.name) || pos+len-12 > 254) return false;
        if (out) q.name[out++] = '.';
        for (size_t i=0; i<len; ++i) {
            uint8_t c = p[pos++];
            if (c <= 32 || c >= 127 || c == '.') return false;
            q.name[out++] = (c >= 'A' && c <= 'Z') ? c+32 : c;
        }
    }
    return false;
}
inline bool skipName(const uint8_t *p, size_t n, size_t &pos) {
    size_t wire = 0;
    while (pos < n) {
        const uint8_t len = p[pos++];
        if (!len) return true;
        if ((len & 0xc0) == 0xc0) {
            if (pos >= n) return false;
            const size_t target = (size_t(len & 0x3f) << 8) | p[pos++];
            return target >= 12 && target < pos-2;
        }
        if (len > 63 || pos+len > n || (wire += len+1) > 254) return false;
        pos += len;
    }
    return false;
}
// Walk all RRs, reject truncated packets, age TTLs without changing OPT flags.
inline bool records(uint8_t *p, size_t n, size_t pos, uint32_t age, uint32_t &minimum) {
    minimum = UINT32_MAX;
    const uint32_t count = uint32_t(u16(p+6))+u16(p+8)+u16(p+10);
    for (uint32_t i=0; i<count; ++i) {
        if (!skipName(p,n,pos) || pos+10 > n) return false;
        const uint16_t type = u16(p+pos), len = u16(p+pos+8);
        if (pos+10+len > n) return false;
        if (type != 41) {
            uint32_t ttl = u32(p+pos+4);
            minimum = std::min(minimum, ttl);
            if (age) put32(p+pos+4, ttl > age ? ttl-age : 0);
        }
        pos += 10+len;
    }
    return pos == n;
}
inline bool query(const uint8_t *p, size_t n, Question &q, uint16_t &udpCap, bool &cacheable) {
    if (!question(p,n,q) || (p[2] & 0x86) || (p[3] & 0xcf) || u16(p+6) || u16(p+8)) return false;
    udpCap = 512;
    cacheable = (p[2] == 1 && p[3] == 0 && u16(p+10) == 0 && n == q.end);
    if (u16(p+10) == 0) return n == q.end;
    // One EDNS(0) OPT RR; retain its bytes when forwarding upstream.
    size_t pos = q.end;
    if (u16(p+10) != 1 || pos+11 > n || p[pos++] != 0 || u16(p+pos) != 41) return false;
    udpCap = std::max<uint16_t>(512, std::min<uint16_t>(1232,u16(p+pos+2)));
    return p[pos+5] == 0 && pos+10+u16(p+pos+8) == n;
}
inline size_t reply(const uint8_t *query, size_t n, uint8_t *out, size_t cap,
                    uint8_t rcode, bool truncated = false) {
    Question q;
    if (!question(query,n,q) || q.end > cap) return 0;
    std::memcpy(out,query,q.end);
    out[2] = 0x80 | (query[2]&1) | (truncated ? 2 : 0);
    out[3] = 0x80 | rcode;
    std::memset(out+6,0,6);
    return q.end;
}
}
