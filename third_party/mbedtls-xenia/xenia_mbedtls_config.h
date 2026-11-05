// Mbed TLS modules xenia builds. Each one needs its library/*.c file listed in
// the mbedtls target in third_party/CMakeLists.txt.
#ifndef XENIA_MBEDTLS_CONFIG_H_
#define XENIA_MBEDTLS_CONFIG_H_

#define MBEDTLS_AES_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_DES_C
#define MBEDTLS_DHM_C
#define MBEDTLS_MD5_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA512_C

#endif  // XENIA_MBEDTLS_CONFIG_H_
