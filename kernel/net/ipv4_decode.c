#include <kernel/ipv4.h>
#include <shared/errno.h>

int ipv4_decode(const void *bytes, size_t length, ipv4_view_t *out) {
    if (length < sizeof(ip_hdr_t)) return -STRATUM_EINVAL;
    const ip_hdr_t *header = bytes;
    uint16_t header_size = (header->ver_ihl & 15) * 4;
    if (header->ver_ihl >> 4 != 4 || header_size < sizeof(*header) || header_size > length) {
        return -STRATUM_EINVAL;
    }
    if (net_checksum(bytes, header_size)) return -STRATUM_EIO;

    uint16_t total = ntohs(header->total_len);
    if (total < header_size || total > length || !header->ttl ||
        (ntohs(header->flags_frag) & 0xBFFF)) return -STRATUM_EINVAL;

    *out = (ipv4_view_t){
        .src = ntohl(header->src_ip), .dst = ntohl(header->dst_ip),
        .header_size = header_size, .payload_size = total - header_size,
        .protocol = header->proto,
    };
    return 0;
}
