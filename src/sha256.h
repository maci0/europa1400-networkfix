#ifndef SHA256_H
#define SHA256_H

// winsock2.h must precede windows.h (mingw warns otherwise)
#include <winsock2.h>
#include <windows.h>

// Bytes needed for a SHA256 digest as a NUL-terminated lowercase hex string.
#define SHA256_HEX_SIZE 65

/**
 * Calculate SHA256 hash of a file using Windows CryptoAPI.
 *
 * @param filepath Path to file to hash (wide character string)
 * @param hash_output Buffer to store hex string result (SHA256_HEX_SIZE bytes)
 * @param output_size Size of hash_output buffer
 * @return TRUE if successful, FALSE otherwise
 */
BOOL calculate_file_sha256(const wchar_t *filepath, char *hash_output, size_t output_size);

#endif // SHA256_H
