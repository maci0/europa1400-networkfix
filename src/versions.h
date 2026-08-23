#ifndef VERSIONS_H
#define VERSIONS_H

#include <windows.h>
#include <winsock2.h>

typedef struct
{
    const char *sha256_hash;
    DWORD       target_rva;
    const char *version_name;
} server_version_info_t;

extern const server_version_info_t known_versions[];

#endif // VERSIONS_H