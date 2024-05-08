#include <kernel/net.h>

/* Return a little-endian word so it can be stored directly in a wire header. */
static uint16_t checksum_with_sum(uint32_t sum, const uint8_t *bytes, size_t length) {
    while (length >= 2) {
        sum += bytes[0] | (uint16_t)bytes[1] << 8;
        bytes += 2;
        length -= 2;
    }
    if (length) sum += bytes[0];
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

uint16_t net_checksum(const void *data, size_t length) {
    return checksum_with_sum(0, data, length);
}

uint16_t net_pseudo_checksum(uint32_t src_ip, uint32_t dst_ip, uint8_t proto, const void *payload, size_t length) {
    uint32_t source = htonl(src_ip), destination = htonl(dst_ip);
    uint32_t sum = (source & 0xffff) + (source >> 16) +
                   (destination & 0xffff) + (destination >> 16) +
                   htons((uint16_t)proto) + htons((uint16_t)length);
    return checksum_with_sum(sum, payload, length);
}
