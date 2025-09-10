#ifndef SHA256_H
#define SHA256_H

#include <windows.h>

/**
 * Calculate SHA256 hash of a file using Windows CryptoAPI.
 *
 * @param filepath Path to file to hash (wide character string)
 * @param hash_output Buffer to store hex string result (must be at least 65 bytes)
 * @param output_size Size of hash_output buffer
 * @return TRUE if successful, FALSE otherwise
 */
BOOL calculate_file_sha256(const wchar_t *filepath, char *hash_output, size_t output_size);

#endif // SHA256_H