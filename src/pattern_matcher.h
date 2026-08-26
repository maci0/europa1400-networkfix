#ifndef PATTERN_MATCHER_H
#define PATTERN_MATCHER_H

// winsock2.h must precede windows.h (mingw warns otherwise)
#include <winsock2.h>
#include <windows.h>

/**
 * Result codes for pattern matching operations.
 */
typedef enum
{
    PATTERN_MATCH_SUCCESS = 0,          // Pattern found and validated successfully
    PATTERN_MATCH_NOT_FOUND = 1,        // Pattern not found in module
    PATTERN_MATCH_INVALID_PARAMS = 2,   // Invalid input parameters
    PATTERN_MATCH_MODULE_ERROR = 3,     // Error getting module information
    PATTERN_MATCH_VALIDATION_FAILED = 4 // Pattern found but failed validation
} PATTERN_MATCH_RESULT;

/**
 * Attempts to find the srv_gameStreamReader function using pattern matching.
 *
 * This function searches for the specific instruction sequence that identifies
 * the srv_gameStreamReader function across different versions of server.dll.
 * The pattern is based on the common prologue and early instructions that
 * are consistent between Steam and GOG versions.
 *
 * @param module_handle Handle to the loaded server.dll module
 * @param found_rva Output parameter to receive the RVA if found (set to 0 on failure)
 * @return PATTERN_MATCH_SUCCESS if found, appropriate error code otherwise
 */
PATTERN_MATCH_RESULT find_srv_gameStreamReader_by_pattern(HMODULE module_handle, DWORD *found_rva);

#ifdef NETWORKFIX_TEST
/* Internals exposed so the Wine test suite can drive them directly. */
long                 find_pattern_in_memory(const unsigned char *haystack, size_t haystack_size,
                                            const unsigned char *needle, const unsigned char *mask,
                                            size_t needle_size);
BOOL                 validate_function_prologue(const unsigned char *base_addr, DWORD rva_offset,
                                                size_t module_size);
PATTERN_MATCH_RESULT find_first_valid_match(const unsigned char *base_addr, size_t module_size,
                                            DWORD *found_rva);
#endif

/**
 * Converts a pattern match result to a human-readable string.
 *
 * @param result The pattern match result code
 * @return String description of the result (never NULL)
 */
const char *pattern_match_result_to_string(PATTERN_MATCH_RESULT result);

#endif // PATTERN_MATCHER_H
