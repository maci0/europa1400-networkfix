#ifndef VERSIONS_H
#define VERSIONS_H

// winsock2.h must precede windows.h (mingw warns otherwise)
#include <winsock2.h>
#include <windows.h>

typedef struct
{
    const char *sha256_hash;
    DWORD       target_rva;
    const char *version_name;
} server_version_info_t;

extern const server_version_info_t known_versions[];

#endif // VERSIONS_H
