#pragma once
#include <cstdint>

// Pure dependency builder shared by production and host tests. Only request IDs
// supported by the active handler. These are RX dependencies, never extra TX.
inline uint8_t buildCanPriorityIds(uint8_t mode, bool mpp, bool apGate, bool sleep,
                                  const uint32_t *all, uint8_t allCount,
                                  uint32_t *out, uint8_t capacity) {
    if (!all || !out) return 0;
    uint8_t count=0;
    auto add=[&](uint32_t id) {
        bool supported=false;
        for (uint8_t i=0; i<allCount; ++i) if (all[i]==id) supported=true;
        for (uint8_t i=0; i<count; ++i) if (out[i]==id) return;
        if (supported && count<capacity) out[count++]=id;
    };
    if (mode==0) { add(0x045); add(0x3EE); if (mpp) add(0x2F8); }
    else { add(0x399); add(0x3F8); add(0x3FD); }
    if (apGate) { add(0x399); add(0x118); add(0x186); add(0x3F8); }
    if (sleep) { add(0x118); add(0x273); add(0x339); }
    return count;
}
