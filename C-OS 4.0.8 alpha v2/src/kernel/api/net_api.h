/**
 * net_api.h - Network API Layer
 * C-OS 4.0.8 alpha
 * 
 * This layer provides a clean interface between GUI/Applications and the
 * underlying network stack (TCP/IP, HTTP, DNS, etc.).
 * This abstracts away low-level details and allows for easier testing and
 * future network stack changes.
 */

#ifndef NET_API_H
#define NET_API_H

#include <stdint.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define NET_MAX_URL 512
#define NET_MAX_HOST 256
#define NET_MAX_PATH 256

/* HTTP methods */
typedef enum {
    NET_HTTP_GET = 0,
    NET_HTTP_POST = 1,
    NET_HTTP_PUT = 2,
    NET_HTTP_DELETE = 3
} net_http_method_t;

/* HTTP response */
typedef struct {
    int status_code;
    char* body;
    uint64_t body_size;
    char* headers;
    uint64_t headers_size;
} net_http_response_t;

/**
 * Initialize the network API layer
 */
void net_api_init(void);

/**
 * Check if network is available
 * @return true if network stack is initialized and ready
 */
bool net_api_is_available(void);

/**
 * Resolve a hostname to an IP address
 * @param hostname Hostname to resolve (e.g., "example.com")
 * @param ip_out Output buffer for IP address (4 bytes for IPv4)
 * @return true on success, false on failure
 */
bool net_api_dns_resolve(const char* hostname, uint8_t* ip_out);

/**
 * Perform an HTTP request
 * @param url Full URL (e.g., "http://example.com/path")
 * @param method HTTP method
 * @param body Request body (can be NULL for GET)
 * @param body_size Size of request body
 * @param response Output response structure (caller must free with net_api_free_response)
 * @return true on success, false on failure
 */
bool net_api_http_request(const char* url, net_http_method_t method, 
                          const char* body, uint64_t body_size,
                          net_http_response_t* response);

/**
 * Free HTTP response resources
 * @param response Response structure to free
 */
void net_api_free_response(net_http_response_t* response);

/**
 * Simple HTTP GET request
 * @param url Full URL to fetch
 * @param buffer Output buffer
 * @param buffer_size Size of buffer
 * @param out_size Actual bytes read (can be NULL)
 * @return true on success, false on failure
 */
bool net_api_http_get(const char* url, char* buffer, uint64_t buffer_size, uint64_t* out_size);

/**
 * Check if a URL uses HTTPS
 * @param url URL to check
 * @return true if HTTPS, false otherwise
 */
bool net_api_is_https(const char* url);

/**
 * Extract hostname from URL
 * @param url Full URL
 * @param host_out Output buffer for hostname
 * @param host_size Size of output buffer
 * @return true on success, false on failure
 */
bool net_api_extract_host(const char* url, char* host_out, size_t host_size);

/**
 * Extract path from URL
 * @param url Full URL
 * @param path_out Output buffer for path
 * @param path_size Size of output buffer
 * @return true on success, false on failure
 */
bool net_api_extract_path(const char* url, char* path_out, size_t path_size);

#endif /* NET_API_H */
