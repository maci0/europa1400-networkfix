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

    if (!filepath || !hash_output || output_size == 0)
    {
        return FALSE;
    }

    // Open file with permissive sharing to avoid Wine deadlock
    hFile =
        CreateFileW(filepath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        log_msg("[SHA256] Failed to open file, error: %lu", GetLastError());
        return FALSE;
    }

    // Get crypto context - PROV_RSA_AES may be unavailable on older Windows;
    // fall back to PROV_RSA_FULL if needed (both support CALG_SHA_256 on modern systems)
    if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
    {
        DWORD err = GetLastError();
        if (err == (DWORD)NTE_BAD_PROV_TYPE || err == (DWORD)NTE_PROV_TYPE_NOT_DEF)
        {
            if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
            {
                log_msg("[SHA256] Failed to acquire crypto context (both AES and FULL), error: %lu",
                        GetLastError());
                goto cleanup;
            }
        }
        else
        {
            log_msg("[SHA256] Failed to acquire crypto context, error: %lu", err);
            goto cleanup;
        }
    }

    // Create hash object
    if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash))
    {
        log_msg("[SHA256] Failed to create hash object, error: %lu", GetLastError());
        goto cleanup;
    }

    // Read and hash file in chunks - check ReadFile success to detect I/O errors
    // 16 KiB balances syscall count against 32-bit thread stack headroom.
    BYTE  buffer[16384];
    DWORD bytesRead;
    while (1)
    {
        if (!ReadFile(hFile, buffer, sizeof(buffer), &bytesRead, NULL))
        {
            log_msg("[SHA256] ReadFile failed, error: %lu", GetLastError());
            goto cleanup;
        }
        if (bytesRead == 0)
            break;
        if (!CryptHashData(hHash, buffer, bytesRead, 0))
        {
            log_msg("[SHA256] Failed to hash data chunk, error: %lu", GetLastError());
            goto cleanup;
        }
    }

    // Get hash result
    BYTE  hashBytes[32]; // SHA256 is 32 bytes
    DWORD hashSize = sizeof(hashBytes);
    if (CryptGetHashParam(hHash, HP_HASHVAL, hashBytes, &hashSize, 0))
    {
        // Need 2 chars per byte + terminator
        if (output_size < (size_t)hashSize * 2 + 1)
        {
            log_msg("[SHA256] Hash aborted: output buffer too small (%zu bytes, need %lu)",
                    output_size, (unsigned long)hashSize * 2 + 1);
        }
        else
        {
            for (DWORD i = 0; i < hashSize; i++)
            {
                sprintf(hash_output + (i * 2), "%02x", hashBytes[i]);
            }
            hash_output[hashSize * 2] = '\0';
            result = TRUE;
        }
    }
    else
    {
        log_msg("[SHA256] Failed to get hash result, error: %lu", GetLastError());
    }

cleanup:
    if (hHash)
    {
        CryptDestroyHash(hHash);
    }
    if (hProv)
    {
        CryptReleaseContext(hProv, 0);
    }
    if (hFile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hFile);
    }
    return result;
}
