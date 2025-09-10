#ifndef VERSIONS_H
#define VERSIONS_H

#include <windows.h>

typedef struct
{
    const char *sha256_hash;
    DWORD       target_rva;
    const char *version_name;
} server_version_info_t;

static const server_version_info_t known_versions[] = {
    {"b341730ba273255fb0099975f30a7b1a950e322be3a491bfd8e137781ac97f06", 0x3720, "German Steam"},
    {"3cc2ce9049e41ab6d0eea042df4966fbf57e5e27c67fb923e81709d2683609d1", 0x3960, "GOG"},
    {NULL, 0, NULL} // Sentinel
};

#endif // VERSIONS_H