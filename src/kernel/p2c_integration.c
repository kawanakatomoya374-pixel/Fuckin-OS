#include "types.h"
#include "memory.h"
#include "string.h"
#include "serial.h"
#include "core/python_code_to_c.h"
#include "platform/python_code_to_c_platform.h"

/* The kernel math object intentionally exposes a small libc surface.  Py2C's
 * optional math module needs these four freestanding-compatible fallbacks. */
extern double exp(double);
extern double log(double);
extern double sqrt(double);
double ldexp(double x, int e) {
    double factor = 1.0;
    if (e < 0) { while (e++ < 0) factor *= 0.5; }
    else { while (e-- > 0) factor *= 2.0; }
    return x * factor;
}
double erf(double x) {
    double ax = x < 0.0 ? -x : x;
    double t = 1.0 / (1.0 + 0.3275911 * ax);
    double p = 1.0 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t * exp(-ax * ax);
    return x < 0.0 ? -p : p;
}
double erfc(double x) { return 1.0 - erf(x); }
double lgamma(double x) {
    if (x <= 0.0) return 0.0;
    if (x < 8.0) { double r = 0.0; while (x < 8.0) { r -= log(x); x += 1.0; } return r + (x - 0.5) * log(x) - x + 0.9189385332046727; }
    return (x - 0.5) * log(x) - x + 0.9189385332046727 + 1.0 / (12.0 * x) - 1.0 / (360.0 * x * x * x);
}
double tgamma(double x) { return exp(lgamma(x)); }

/* Py2C's freestanding core uses the C-OS kernel heap through the normal
 * malloc/realloc/free compatibility layer.  The bridge keeps the public
 * kernel/UI contract deliberately small and bounded. */
#define P2C_COS_OUTPUT_MAX (64u * 1024u)

static void *p2c_cos_alloc(size_t size, void *user) {
    (void)user;
    return kmalloc(size);
}
static void *p2c_cos_realloc(void *ptr, size_t old_size, size_t new_size, void *user) {
    (void)old_size; (void)user;
    return krealloc(ptr, new_size);
}
static void p2c_cos_free(void *ptr, size_t size, void *user) {
    (void)size; (void)user;
    kfree(ptr);
}

void p2c_cos_init(void) {
    p2c_platform_set_allocator(p2c_cos_alloc, p2c_cos_realloc, p2c_cos_free, NULL);
}

int p2c_cos_convert(const char *source, char *output, size_t output_size,
                    char *error, size_t error_size) {
    if (!source || !output || output_size < 2u) return -1;
    output[0] = '\0';
    if (error && error_size) error[0] = '\0';
    p2c_cos_init();
    char *generated = NULL;
    P2C_Result result = python_to_c(source, NULL, &generated);
    if (result != P2C_OK || !generated) {
        const char *detail = p2c_last_error_details();
        if (error && error_size && detail) {
            size_t i = 0;
            while (i + 1u < error_size && detail[i]) { error[i] = detail[i]; i++; }
            error[i] = '\0';
        }
        if (generated) kfree(generated);
        return (int)result ? (int)result : -1;
    }
    size_t i = 0;
    while (i + 1u < output_size && generated[i]) { output[i] = generated[i]; i++; }
    output[i] = '\0';
    kfree(generated);
    if (i + 1u >= output_size && error && error_size) {
        const char *msg = "Generated C output exceeded the C-OS preview buffer";
        size_t j = 0; while (j + 1u < error_size && msg[j]) { error[j] = msg[j]; j++; }
        error[j] = '\0';
        return -2;
    }
    return 0;
}

const char *p2c_cos_supported(void) {
    return p2c_supported_range_string();
}
