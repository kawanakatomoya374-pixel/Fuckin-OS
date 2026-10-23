#include "runtime/python_code_to_c_runtime.h"
#include "platform/python_code_to_c_platform.h"

#include <string.h>

static char captured[64];
static size_t captured_len;

static void capture_write(int stream, const char *data, size_t len, void *user) {
    (void)stream;
    (void)user;
    if (!data || len == 0) return;
    if (len > sizeof(captured) - 1 - captured_len) len = sizeof(captured) - 1 - captured_len;
    memcpy(captured + captured_len, data, len);
    captured_len += len;
    captured[captured_len] = '\0';
}

int main(void) {
    P2C_Platform platform = *p2c_platform_default();
    platform.write = capture_write;
    p2c_platform_set(&platform);

    P2C_Object *args[2];
    args[0] = p2c_obj_from_int(7);
    args[1] = p2c_obj_from_int(9);
    (void)p2c_print_multi_opts(args, 2, p2c_obj_from_str("|"), p2c_obj_from_str("!"));
    p2c_platform_set(NULL);

    if (strcmp(captured, "7|9!") != 0) return 1;
    return 0;
}
