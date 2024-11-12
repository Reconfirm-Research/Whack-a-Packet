#include "../include/af_xdp_init.h"
#include "../include/dns_query.h"
#include "../include/cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <numa.h>
#include <sched.h>
#include <pthread.h>
#include <sys/types.h>
#include <linux/if_link.h>
#include <linux/if_xdp.h>
#include <xdp/xsk.h>
#include <xdp/libxdp.h>

// Global variables for program control
static volatile int running = 1;
static struct xdp_socket xsk = {0};

// Configuration structure
struct config {
    char *interface;
    char *domains_file;
    char *resolvers_file;
    unsigned int rate_limit;
    char *output_file;
    size_t cache_size;
    unsigned int cache_ttl;
    int numa_node;
    int cpu_core;
};

// Signal handler for graceful shutdown
static void signal_handler(int signum) {
    (void)signum;  // Unused parameter
    running = 0;
}

// Process received DNS packet
static void process_packet(const uint8_t *packet, size_t length) {
    struct dns_query query;
    uint8_t response[512];
    size_t response_len = sizeof(response);

    // First check if this is a DNS query
    if (length < sizeof(struct dns_header)) {
        return;
    }

    // Parse the DNS header
    memcpy(&query.header, packet, sizeof(struct dns_header));

    // Check cache first
    if (cache_lookup((char*)(packet + sizeof(struct dns_header)), response, &response_len)) {
        // Send cached response
        af_xdp_socket_tx(&xsk, response, response_len);
        return;
    }

    // Process the query and prepare response
    if (parse_response(packet, length, &query) == 0) {
        // Cache the response for future use
        cache_insert((char*)(packet + sizeof(struct dns_header)), 
                    response, response_len, 
                    3600); // Default TTL of 1 hour
        
        // Send the response
        af_xdp_socket_tx(&xsk, response, response_len);
    }
}

// Initialize program configuration
static void init_config(struct config *cfg) {
    memset(cfg, 0, sizeof(struct config));
    cfg->cache_size = 10000;    // Default cache size
    cfg->cache_ttl = 3600;      // Default TTL: 1 hour
    cfg->rate_limit = 5000;     // Default rate limit: 5000 queries/sec
    cfg->numa_node = -1;        // Auto-detect NUMA node
    cfg->cpu_core = -1;         // Auto-detect CPU core
}

// Set CPU affinity for optimal performance
static int set_cpu_affinity(int cpu_core) {
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(cpu_core, &cpu_set);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set), &cpu_set);
}

// Parse command line arguments
static int parse_args(int argc, char **argv, struct config *cfg) {
    static struct option long_options[] = {
        {"interface", required_argument, 0, 'i'},
        {"domains", required_argument, 0, 'd'},
        {"resolvers", required_argument, 0, 'r'},
        {"rate-limit", required_argument, 0, 'l'},
        {"output", required_argument, 0, 'o'},
        {"cache-size", required_argument, 0, 'c'},
        {"numa-node", required_argument, 0, 'n'},
        {"cpu-core", required_argument, 0, 'p'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "i:d:r:l:o:c:n:p:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'i':
                cfg->interface = optarg;
                break;
            case 'd':
                cfg->domains_file = optarg;
                break;
            case 'r':
                cfg->resolvers_file = optarg;
                break;
            case 'l':
                cfg->rate_limit = atoi(optarg);
                break;
            case 'o':
                cfg->output_file = optarg;
                break;
            case 'c':
                cfg->cache_size = atoi(optarg);
                break;
            case 'n':
                cfg->numa_node = atoi(optarg);
                break;
            case 'p':
                cfg->cpu_core = atoi(optarg);
                break;
            case 'h':
                printf("Usage: %s -i <interface> -d <domains_file> -r <resolvers_file> [options]\n", argv[0]);
                printf("Options:\n");
                printf("  -i, --interface    Network interface to use\n");
                printf("  -d, --domains      File containing domains to resolve\n");
                printf("  -r, --resolvers    File containing DNS resolvers\n");
                printf("  -l, --rate-limit   Query rate limit (default: 5000)\n");
                printf("  -o, --output       Output file for results\n");
                printf("  -c, --cache-size   Cache size (default: 10000)\n");
                printf("  -n, --numa-node    NUMA node to use (default: auto)\n");
                printf("  -p, --cpu-core     CPU core to use (default: auto)\n");
                printf("  -h, --help         Show this help message\n");
                return 1;
            default:
                return -1;
        }
    }

    // Validate required arguments
    if (!cfg->interface || !cfg->domains_file || !cfg->resolvers_file) {
        fprintf(stderr, "Missing required arguments\n");
        return -1;
    }

    return 0;
}

int main(int argc, char **argv) {
    struct config cfg;
    struct cache_config cache_cfg;
    struct xdp_socket_config xsk_cfg;

    // Initialize configuration
    init_config(&cfg);

    // Parse command line arguments
    if (parse_args(argc, argv, &cfg) != 0) {
        return 1;
    }

    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize cache
    cache_cfg.max_entries = cfg.cache_size;
    cache_cfg.default_ttl = cfg.cache_ttl;
    cache_cfg.cleanup_interval = 60;  // Cleanup every minute
    cache_init(&cache_cfg);

    // Configure XDP socket
    memset(&xsk_cfg, 0, sizeof(xsk_cfg));
    xsk_cfg.rx_size = XSK_RING_SIZE;
    xsk_cfg.tx_size = XSK_RING_SIZE;
    xsk_cfg.batch_size = XSK_BATCH_SIZE;
    xsk_cfg.bind_flags = XDP_USE_NEED_WAKEUP;
    xsk_cfg.xdp_flags = true;  // Use native mode if available
    xsk_cfg.ifname = cfg.interface;

    // Initialize AF_XDP socket
    if (af_xdp_socket_init(&xsk, &xsk_cfg) != 0) {
        fprintf(stderr, "Failed to initialize AF_XDP socket\n");
        return 1;
    }

    // Set CPU affinity if specified
    if (cfg.cpu_core >= 0) {
        if (set_cpu_affinity(cfg.cpu_core) != 0) {
            fprintf(stderr, "Warning: Failed to set CPU affinity\n");
        }
    }

    printf("whack started on interface %s\n", cfg.interface);
    printf("Cache size: %zu entries\n", cfg.cache_size);
    printf("Rate limit: %u queries/sec\n", cfg.rate_limit);
    if (cfg.cpu_core >= 0) {
        printf("CPU core: %d\n", cfg.cpu_core);
    }
    if (cfg.numa_node >= 0) {
        printf("NUMA node: %d\n", cfg.numa_node);
    }

    // Main processing loop
    while (running) {
        // Poll for packets
        if (af_xdp_socket_poll(&xsk, 1000) > 0) {
            // Process received packets
            af_xdp_socket_rx(&xsk, process_packet);
        }

        // Periodic cache cleanup
        static time_t last_cleanup = 0;
        time_t now = time(NULL);
        if (now - last_cleanup >= cache_cfg.cleanup_interval) {
            cache_cleanup();
            last_cleanup = now;
        }
    }

    // Cleanup
    printf("\nShutting down...\n");
    af_xdp_socket_cleanup(&xsk);
    cache_destroy();

    // Print statistics
    printf("Cache statistics:\n");
    printf("  Hits: %zu\n", cache_get_hit_count());
    printf("  Misses: %zu\n", cache_get_miss_count());
    printf("  Hit ratio: %.2f%%\n", cache_get_hit_ratio() * 100);

    return 0;
}
