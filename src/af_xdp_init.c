#include "../include/af_xdp_init.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <poll.h>
#include <unistd.h>

#ifndef SOL_XDP
#define SOL_XDP 283
#endif

static int xsk_configure_umem(struct xdp_socket *xsk_socket) {
    struct xsk_umem_config umem_cfg = {
        .fill_size = XSK_RING_SIZE,
        .comp_size = XSK_RING_SIZE,
        .frame_size = XSK_UMEM_FRAME_SIZE,
        .frame_headroom = XSK_UMEM__DEFAULT_FRAME_HEADROOM,
        .flags = 0
    };

    // Try to allocate huge pages first
    void *bufs = mmap(NULL, 
                     XSK_UMEM_FRAME_SIZE * XSK_NUM_FRAMES,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                     -1, 0);
    
    if (bufs == MAP_FAILED) {
        // Fallback to regular pages
        bufs = mmap(NULL, 
                   XSK_UMEM_FRAME_SIZE * XSK_NUM_FRAMES,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS,
                   -1, 0);
        
        if (bufs == MAP_FAILED) {
            return -errno;
        }
    }

    // Create and configure UMEM
    int ret = xsk_umem__create(&xsk_socket->umem,
                              bufs,
                              XSK_UMEM_FRAME_SIZE * XSK_NUM_FRAMES,
                              &xsk_socket->fq,
                              &xsk_socket->cq,
                              &umem_cfg);
    
    if (ret) {
        munmap(bufs, XSK_UMEM_FRAME_SIZE * XSK_NUM_FRAMES);
        return ret;
    }

    xsk_socket->buffer = bufs;
    return 0;
}

int af_xdp_socket_init(struct xdp_socket *xsk_socket, struct xdp_socket_config *config) {
    // Zero out the socket structure
    memset(xsk_socket, 0, sizeof(*xsk_socket));

    // Get interface index
    xsk_socket->ifindex = if_nametoindex(config->ifname);
    if (!xsk_socket->ifindex) {
        return -errno;
    }

    // Configure UMEM
    int ret = xsk_configure_umem(xsk_socket);
    if (ret) {
        return ret;
    }

    // Configure socket
    struct xsk_socket_config xsk_cfg = {
        .rx_size = config->rx_size,
        .tx_size = config->tx_size,
        .libbpf_flags = 0,
        .xdp_flags = config->xdp_flags ? XDP_FLAGS_UPDATE_IF_NOEXIST | XDP_FLAGS_DRV_MODE : 0,
        .bind_flags = config->bind_flags | XDP_USE_NEED_WAKEUP
    };

    // Create XDP socket
    ret = xsk_socket__create(&xsk_socket->xsk,
                            config->ifname,
                            0, // Queue ID
                            xsk_socket->umem,
                            &xsk_socket->rx,
                            &xsk_socket->tx,
                            &xsk_cfg);
    
    if (ret) {
        af_xdp_socket_cleanup(xsk_socket);
        return ret;
    }

    // Increase resource limits for performance
    struct rlimit rlim = {RLIM_INFINITY, RLIM_INFINITY};
    setrlimit(RLIMIT_MEMLOCK, &rlim);

    return 0;
}

void af_xdp_socket_rx(struct xdp_socket *xsk_socket, void (*process_packet)(const uint8_t *, size_t)) {
    unsigned int rcvd, i;
    uint32_t idx_rx = 0;

    // Receive packets in batches
    rcvd = xsk_ring_cons__peek(&xsk_socket->rx, XSK_BATCH_SIZE, &idx_rx);
    if (!rcvd)
        return;

    // Process received packets
    for (i = 0; i < rcvd; i++) {
        const struct xdp_desc *desc = xsk_ring_cons__rx_desc(&xsk_socket->rx, idx_rx++);
        uint64_t addr = xsk_umem__extract_addr(desc->addr);
        uint32_t len = desc->len;
        uint8_t *pkt = xsk_umem__get_data(xsk_socket->buffer, addr);

        // Process the packet
        if (process_packet) {
            process_packet(pkt, len);
        }
    }

    // Release processed packets
    xsk_ring_cons__release(&xsk_socket->rx, rcvd);

    // Complete any pending transmissions
    af_xdp_socket_complete_tx(xsk_socket);
}

int af_xdp_socket_tx(struct xdp_socket *xsk_socket, const uint8_t *pkt, size_t len) {
    uint32_t idx_tx;
    struct xdp_desc *desc;

    // Reserve space in the TX ring
    if (xsk_ring_prod__reserve(&xsk_socket->tx, 1, &idx_tx) != 1)
        return -ENOSPC;

    // Get the descriptor and copy the packet
    desc = xsk_ring_prod__tx_desc(&xsk_socket->tx, idx_tx);
    desc->addr = (uint64_t)pkt - (uint64_t)xsk_socket->buffer;
    desc->len = len;

    // Submit the packet for transmission
    xsk_ring_prod__submit(&xsk_socket->tx, 1);
    xsk_socket->outstanding_tx++;

    // Kick the kernel if needed
    if (xsk_ring_prod__needs_wakeup(&xsk_socket->tx)) {
        sendto(xsk_socket__fd(xsk_socket->xsk), NULL, 0, MSG_DONTWAIT, NULL, 0);
    }

    return 0;
}

void af_xdp_socket_complete_tx(struct xdp_socket *xsk_socket) {
    unsigned int completed;
    uint32_t idx_cq;

    if (!xsk_socket->outstanding_tx)
        return;

    // Process completed transmissions
    completed = xsk_ring_cons__peek(&xsk_socket->cq, XSK_BATCH_SIZE, &idx_cq);
    if (completed > 0) {
        xsk_ring_cons__release(&xsk_socket->cq, completed);
        xsk_socket->outstanding_tx -= completed;
    }
}

int af_xdp_socket_poll(struct xdp_socket *xsk_socket, int timeout_ms) {
    struct pollfd fds = {
        .fd = xsk_socket__fd(xsk_socket->xsk),
        .events = POLLIN,
    };

    return poll(&fds, 1, timeout_ms);
}

void af_xdp_socket_cleanup(struct xdp_socket *xsk_socket) {
    if (!xsk_socket)
        return;

    // Complete any pending transmissions
    while (xsk_socket->outstanding_tx > 0) {
        af_xdp_socket_complete_tx(xsk_socket);
    }

    // Cleanup XDP socket
    if (xsk_socket->xsk) {
        xsk_socket__delete(xsk_socket->xsk);
    }

    // Cleanup UMEM
    if (xsk_socket->umem) {
        xsk_umem__delete(xsk_socket->umem);
    }

    // Free packet buffer memory
    if (xsk_socket->buffer) {
        munmap(xsk_socket->buffer, XSK_UMEM_FRAME_SIZE * XSK_NUM_FRAMES);
    }

    memset(xsk_socket, 0, sizeof(*xsk_socket));
}
