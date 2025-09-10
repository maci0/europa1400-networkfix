#include "sha256.h"
#include "logging.h"
#include <stdio.h>
#include <wincrypt.h>
#include <windows.h>

/**
 * Calculate SHA256 hash of a file using Windows CryptoAPI.
 * Returns lowercase hex string of SHA256 hash.
 */
BOOL calculate_file_sha256(const wchar_t *filepath, char *hash_output, size_t output_size)
{
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    HANDLE     hFile = INVALID_HANDLE_VALUE;
    BOOL       result = FALSE;

    logf("[SHA256] Starting hash calculation for file");

    // Open file with permissive sharing to avoid Wine deadlock
    hFile = CreateFileW(filepath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                        OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        logf("[SHA256] Failed to open file, error: %lu", GetLastError());
        return FALSE;
    }

    logf("[SHA256] File opened successfully, handle: %p", (void *)hFile);

    // Get crypto context
    if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
    {
        logf("[SHA256] Failed to acquire crypto context, error: %lu", GetLastError());
        goto cleanup;
    }
    logf("[SHA256] Crypto context acquired successfully");

    // Create hash object
    if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash))
    {
        logf("[SHA256] Failed to create hash object, error: %lu", GetLastError());
        goto cleanup;
    }
    logf("[SHA256] Hash object created successfully");

    // Read and hash file in chunks
    BYTE  buffer[4096];
    DWORD bytesRead;
    DWORD totalBytesRead = 0;

    logf("[SHA256] Starting file read loop");
    while (ReadFile(hFile, buffer, sizeof(buffer), &bytesRead, NULL) && bytesRead > 0)
    {
        totalBytesRead += bytesRead;
        if (!CryptHashData(hHash, buffer, bytesRead, 0))
        {
            logf("[SHA256] Failed to hash data chunk, error: %lu", GetLastError());
            goto cleanup;
        }
    }
    logf("[SHA256] File read completed, total bytes: %lu", totalBytesRead);

    // Get hash result
    BYTE  hashBytes[32]; // SHA256 is 32 bytes
    DWORD hashSize = sizeof(hashBytes);
    if (CryptGetHashParam(hHash, HP_HASHVAL, hashBytes, &hashSize, 0))
    {
        logf("[SHA256] Hash calculation successful, converting to hex string");
        // Convert to hex string
        for (DWORD i = 0; i < hashSize && (i * 2 + 1) < output_size; i++)
        {
            sprintf(hash_output + (i * 2), "%02x", hashBytes[i]);
        }
        hash_output[hashSize * 2] = '\0';
        result = TRUE;
        logf("[SHA256] Hash conversion completed successfully");
    }
    else
    {
        logf("[SHA256] Failed to get hash result, error: %lu", GetLastError());
    }

cleanup:
    logf("[SHA256] Starting cleanup - hash: %p, provider: %p, file: %p", (void *)hHash, (void *)hProv, (void *)hFile);

    if (hHash)
    {
        CryptDestroyHash(hHash);
        logf("[SHA256] Hash object destroyed");
    }
    if (hProv)
    {
        CryptReleaseContext(hProv, 0);
        logf("[SHA256] Crypto context released");
    }
    if (hFile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hFile);
        logf("[SHA256] File handle closed");
    }

    logf("[SHA256] Cleanup completed, returning: %s", result ? "TRUE" : "FALSE");
    return result;
}