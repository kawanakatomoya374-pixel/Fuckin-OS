C-OS OpenSSL subset

This directory vendors the OpenSSL source tree subset required for a hosted TLS backend.
The freestanding kernel build still defaults to BearSSL. Enable the OpenSSL backend only
when building in an environment that can link against OpenSSL and a hosted socket layer.
