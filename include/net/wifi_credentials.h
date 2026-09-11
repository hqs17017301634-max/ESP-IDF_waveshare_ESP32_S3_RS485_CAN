#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace wifi_credentials {
// IDF's SSID and raw PSK arrays permit a full-width value without a terminator.
inline bool copy(uint8_t (&ssidOut)[32], uint8_t (&passOut)[64],
                 const char *ssid, const char *pass) {
    if (!ssid || !pass) return false;
    const size_t sn = std::strlen(ssid), pn = std::strlen(pass);
    if (!sn || sn > 32 || pn > 64 || (pn && pn < 8)) return false;
    if (pn == 64) {
        for (size_t i = 0; i < pn; ++i) {
            const char c = pass[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F'))) return false;
        }
    }
    std::memset(ssidOut, 0, sizeof(ssidOut));
    std::memset(passOut, 0, sizeof(passOut));
    std::memcpy(ssidOut, ssid, sn);
    std::memcpy(passOut, pass, pn);
    return true;
}
}
