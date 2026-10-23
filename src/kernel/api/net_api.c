/**
 * net_api.c - Network API Layer Implementation
 * C-OS 4.0.8 alpha
 * 
 * This layer wraps the existing network stack (http.c, dns.c, etc.) to provide
 * a clean interface between GUI/Applications and network functionality.
 */

#include "net_api.h"
#include "../drivers/http.h"
#include "../drivers/dns.h"
#include "../drivers/net.h"
#include "../../include/memory.h"
#include "../../include/string.h"
#include <string.h>

static bool g_net_api_initialized = false;

void net_api_init(void) {
    if (!g_net_api_initialized) {
        dns_init();
        g_net_api_initialized = true;
    }
}

bool net_api_is_available(void) {
    if (!g_net_api_initialized) net_api_init();
    return net_is_connected();
}

bool net_api_dns_resolve(const char* hostname, uint8_t* ip_out) {
    if (!g_net_api_initialized) net_api_init();
    if (!hostname || !ip_out) return false;
    
    ip_addr_t result;
    if (dns_resolve(hostname, &result) != 0) return false;
    
    // Copy IP address to output buffer (assuming IPv4)
    ip_out[0] = result.addr[0];
    ip_out[1] = result.addr[1];
    ip_out[2] = result.addr[2];
    ip_out[3] = result.addr[3];
    
    return true;
}

bool net_api_http_request(const char* url, net_http_method_t method,
                          const char* body, uint64_t body_size,
                          net_http_response_t* response) {
    if (!g_net_api_initialized) net_api_init();
    if (!url || !response) return false;
    (void)body_size;

    memset(response, 0, sizeof(*response));

    http_client_t* http = http_create();
    if (!http) return false;
    
    int result = 0;
    switch (method) {
        case NET_HTTP_GET:
            result = http_get(http, url);
            break;
        case NET_HTTP_POST:
            result = http_post(http, url, body ? body : "");
            break;
        default:
            http_destroy(http);
            return false;
    }
    
    if (result != 0) {
        http_destroy(http);
        return false;
    }
    
    response->status_code = http_status_code(http);
    const char* resp_body = http_response_body(http);
    uint64_t resp_len = http_response_length(http);
    
    if (resp_body && resp_len > 0) {
        response->body = (char*)kmalloc(resp_len + 1);
        if (response->body) {
            memcpy(response->body, resp_body, resp_len);
            response->body[resp_len] = '\0';
            response->body_size = resp_len;
        } else {
            http_destroy(http);
            return false;
        }
    }
    
    http_destroy(http);
    return true;
}

void net_api_free_response(net_http_response_t* response) {
    if (response) {
        if (response->body) {
            kfree(response->body);
            response->body = NULL;
        }
        if (response->headers) {
            kfree(response->headers);
            response->headers = NULL;
        }
        response->body_size = 0;
        response->headers_size = 0;
        response->status_code = 0;
    }
}

bool net_api_http_get(const char* url, char* buffer, uint64_t buffer_size, uint64_t* out_size) {
    if (!g_net_api_initialized) net_api_init();
    if (!url || !buffer || buffer_size == 0) return false;
    
    // Use the simple http_fetch API
    int result = http_fetch(url, buffer, (size_t)buffer_size);
    if (result != 0) return false;
    
    if (out_size) {
        *out_size = strlen(buffer);
    }
    
    return true;
}

bool net_api_is_https(const char* url) {
    if (!url) return false;
    
    // Check if URL starts with "https://"
    const char* https_prefix = "https://";
    const char* http_prefix = "http://";
    
    if (strncmp(url, https_prefix, 8) == 0) return true;
    if (strncmp(url, http_prefix, 7) == 0) return false;
    
    // Default to HTTP if no scheme specified
    return false;
}

bool net_api_extract_host(const char* url, char* host_out, size_t host_size) {
    if (!url || !host_out || host_size == 0) return false;
    
    char host[HTTP_MAX_HOST];
    char path[HTTP_MAX_PATH];
    uint64_t port;
    
    if (http_parse_url(url, host, path, &port) != 0) return false;
    
    strncpy(host_out, host, host_size - 1);
    host_out[host_size - 1] = '\0';
    
    return true;
}

bool net_api_extract_path(const char* url, char* path_out, size_t path_size) {
    if (!url || !path_out || path_size == 0) return false;
    
    char host[HTTP_MAX_HOST];
    char path[HTTP_MAX_PATH];
    uint64_t port;
    
    if (http_parse_url(url, host, path, &port) != 0) return false;
    
    strncpy(path_out, path, path_size - 1);
    path_out[path_size - 1] = '\0';
    
    return true;
}
