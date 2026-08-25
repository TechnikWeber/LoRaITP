/*
 * CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) over the whole image.
 *
 * Computed incrementally so the sender never needs the image in RAM and
 * the receiver can verify while reassembling.
 *
 * The per-frame integrity check is the PHY's own CRC-16 in hardware; this
 * one exists to catch the rare undetected PHY error and to prove that
 * reassembly put the chunks back in the right order.
 */
#include "loraitp.h"

#define POLY 0xEDB88320u

uint32_t loraitp_crc32_update(uint32_t crc, const uint8_t *buf, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (POLY & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return ~crc;
}

uint32_t loraitp_crc32(const uint8_t *buf, size_t len)
{
    return loraitp_crc32_update(0, buf, len);
}
