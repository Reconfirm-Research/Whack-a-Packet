#ifndef AF_XDP_INIT_H
#define AF_XDP_INIT_H

#include <linux/if_xdp.h>
#include <linux/if_link.h>
#include <xdp/libxdp.h>
#include <xdp/xsk.h>
#include <net/if.h>
#include <stdint.h>
#include <stdbool.h>

// Default configuration values
#define XSK_RING_SIZE       4096
#define XSK_BATCH_SIZE      64
#define XSK_UMEM_FRAME_SIZE 2048
#define XSK_NUM_FRAMES      4096

// XDP flags if not defined
#ifndef XDP_FLAGS_UPDATE_IF_NOEXIST
#define XDP_FLAGS_UPDATE_IF_NOEXIST (1U << 0)
#endif

#ifndef XDP_FLAGS_DRV_MODE
#define XDP_FLAGS_DRV_MODE (0U << 1)
#endif

#ifndef XDP_USE_NEED_WAKEUP
#define XDP_USE_NEED_WAKEUP (1U << 3)
#endif

// Structure to hold XDP socket information
struct xdp_socket {
    int ifindex;                    // Interface index
    struct xsk_socket *xsk;         // XDP socket
    struct xsk_umem *umem;          // UMEM area
    struct xsk_ring_prod fq;        // Fill queue
    struct xsk_ring_cons cq;        // Completion queue
    struct xsk_ring_prod tx;        // TX ring
    struct xsk_ring_cons rx;        // RX ring
    void *buffer;                   // Packet buffer
    __u32 prog_id;                  // XDP program ID
    unsigned int outstanding_tx;     // Number of outstanding TX packets
};

// XDP socket configuration
struct xdp_socket_config {
    __u32 rx_size;                  // RX ring size
    __u32 tx_size;                  // TX ring size
    __u32 batch_size;               // Batch size for processing
    int bind_flags;                 // Socket bind flags
    bool xdp_flags;                 // XDP program flags
    char *ifname;                   // Interface name
};

// Function declarations
int af_xdp_socket_init(struct xdp_socket *xsk_socket, struct xdp_socket_config *config);
void af_xdp_socket_rx(struct xdp_socket *xsk_socket, void (*process_packet)(const uint8_t *, size_t));
int af_xdp_socket_tx(struct xdp_socket *xsk_socket, const uint8_t *pkt, size_t len);
void af_xdp_socket_cleanup(struct xdp_socket *xsk_socket);

// Helper functions
int af_xdp_socket_poll(struct xdp_socket *xsk_socket, int timeout_ms);
void af_xdp_socket_complete_tx(struct xdp_socket *xsk_socket);

#endif // AF_XDP_INIT_H
