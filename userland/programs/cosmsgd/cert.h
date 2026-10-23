#ifndef COSMSGD_CERT_H
#define COSMSGD_CERT_H
#include <stddef.h>
#include <stdbool.h>
/* Makes a self-signed ECDSA P-256 certificate (CN=cosmsg, valid until 2100). der must hold 512 bytes; key gets the 32-byte private scalar. */
bool cert_make(unsigned char *der, size_t *der_len, unsigned char key[32]);
#endif
