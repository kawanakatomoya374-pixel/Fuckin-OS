#ifndef COS_BROWSER_CORE_H
#define COS_BROWSER_CORE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    COS_BROWSER_URL_MAX = 512,
    COS_BROWSER_URL_KEY_NONE = 0,
    COS_BROWSER_URL_KEY_COMMIT = 1,
    COS_BROWSER_URL_KEY_CANCEL = 2
};

int cos_browser_normalize_address(const char* raw_url, char* url, size_t url_sz);
int cos_browser_build_google_search_url(const char* raw_query, char* url, size_t url_sz);
int cos_browser_handle_url_key(char* buffer, size_t buffer_sz, char ascii, int scancode);
int cos_browser_history_push(char history[][COS_BROWSER_URL_MAX], int* count, int* pos, int max_count, const char* url);
int cos_browser_history_move(int* pos, int count, int delta);

#ifdef __cplusplus
}
#endif

#endif /* COS_BROWSER_CORE_H */
