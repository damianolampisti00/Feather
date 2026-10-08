mbed TLS 2.28.9 (LTS), https://github.com/Mbed-TLS/mbedtls
Apache License 2.0 (LICENSE).

include/: the release headers; lib/armv7/libmbedtls_all.a: libmbedcrypto +
libmbedx509 + libmbedtls in one static archive, cross-compiled with the
BlackBerry 10 NDK (qcc, armv7) for the BBport project. Used by the client's
TLS 1.2 transport (client/src/tls): BB10's own OpenSSL only speaks TLS 1.0.
