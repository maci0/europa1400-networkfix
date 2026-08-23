/*
 * pattern_matcher.c: Pattern matching for finding function offsets in server.dll
 *
 * This module provides pattern matching functionality to locate the target
 * srv_gameStreamReader function across different versions of server.dll without
 * relying on fixed offsets or file checksums.
 */

#include "pattern_matcher.h"
#include "logging.h"
#include <psapi.h>
#include <string.h>
#include <windows.h>

#ifdef NETWORKFIX_TEST
#define PATTERN_STATIC
#else
#define PATTERN_STATIC static
#endif

// Pattern for srv_gameStreamReader function based on disassembly analysis
// Common signature across Steam and GOG versions:
static const unsigned char SRV_GAMESTREAMREADER_PATTERN[] = {
    0x51,                               // PUSH ECX
    0x8B, 0x4C, 0x24, 0x0C,             // MOV ECX,dword ptr [ESP + 0x0C]
    0x53,                               // PUSH EBX
    0x55,                               // PUSH EBP
    0x8B, 0x6C, 0x24, 0x10,             // MOV EBP,dword ptr [ESP + 0x10]
    0x56,                               // PUSH ESI
    0x57,                               // PUSH EDI
    0x85, 0xED,                         // TEST EBP,EBP
    0x8B, 0xF1,                         // MOV ESI,ECX
    0x0F, 0x84, 0x00, 0x00, 0x00, 0x00, // JZ (wildcard - offset varies)
    0x80, 0x7D, 0x5C, 0x72,             // CMP byte ptr [EBP + 0x5c],0x72
    0x0F, 0x85, 0x00, 0x00, 0x00, 0x00, // JNZ (wildcard - offset varies)
    0x8B, 0x45, 0x38                    // MOV EAX,dword ptr [EBP + 0x38]
};

// Mask to indicate which bytes are exact matches (0xFF) vs wildcards (0x00)
static const unsigned char SRV_GAMESTREAMREADER_MASK[] = {
    0xFF,                               // PUSH ECX (exact)
    0xFF, 0xFF, 0xFF, 0xFF,             // MOV ECX,dword ptr [ESP + 0x0C] (exact)
    0xFF,                               // PUSH EBX (exact)
    0xFF,                               // PUSH EBP (exact)
    0xFF, 0xFF, 0xFF, 0xFF,             // MOV EBP,dword ptr [ESP + 0x10] (exact)
    0xFF,                               // PUSH ESI (exact)
    0xFF,                               // PUSH EDI (exact)
    0xFF, 0xFF,                         // TEST EBP,EBP (exact)
    0xFF, 0xFF,                         // MOV ESI,ECX (exact)
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, // JZ (opcode exact, offset wildcard)
    0xFF, 0xFF, 0xFF, 0xFF,             // CMP byte ptr [EBP + 0x5c],0x72 (exact)
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, // JNZ (opcode exact, offset wildcard)
    0xFF, 0xFF, 0xFF                    // MOV EAX,dword ptr [EBP + 0x38] (exact)
};

#define SRV_GAMESTREAMREADER_PATTERN_SIZE (sizeof(SRV_GAMESTREAMREADER_PATTERN))

/**
 * Validates that the rel32 target of a two-byte-opcode branch (0F 84/85)
 * starting at `opcode_off` bytes into the function stays inside the module.
 * The operand follows the opcode; the next instruction begins 6 bytes after
 * it. Signed offsets can point backwards, but a target below the module base
 * or past its end means the match is not a real function entry.
 *
 * @return TRUE if the target is within [0, module_size), FALSE otherwise
 */
static BOOL branch_target_in_bounds(const unsigned char *func_start, size_t opcode_off,
                                    DWORD rva_offset, size_t module_size)
{
    int32_t rel;
    memcpy(&rel, func_start + opcode_off + 2, sizeof(rel));
    int64_t target = (int64_t)rva_offset + (int64_t)opcode_off + 6 + (int64_t)rel;
    if (target < 0 || (uint64_t)target >= module_size)
    {
        // Signed so backwards (negative rel32) targets are not printed as
        // huge hex addresses.
        log_msg("[PATTERN] Branch target %lld is beyond module bounds [0, 0x%zX)",
                (long long)target, module_size);
        return FALSE;
    }
    return TRUE;
}

/**
 * Searches for a byte pattern within a memory region using mask-based matching.
 *
 * @param haystack Pointer to the memory region to search
 * @param haystack_size Size of the memory region in bytes
 * @param needle Pattern bytes to search for
 * @param mask Mask indicating which bytes must match exactly (0xFF) vs wildcards (0x00)
 * @param needle_size Size of the pattern in bytes
 * @return Offset from haystack start if found, or -1 if not found
 */
PATTERN_STATIC long find_pattern_in_memory(const unsigned char *haystack, size_t haystack_size,
                                           const unsigned char *needle, const unsigned char *mask,
                                           size_t needle_size)
{
    if (!haystack || !needle || !mask || needle_size == 0 || haystack_size < needle_size)
    {
        return -1;
    }

    for (size_t i = 0; i <= haystack_size - needle_size; i++)
    {
        BOOL match = TRUE;
        for (size_t j = 0; j < needle_size; j++)
        {
            if (mask[j] == 0xFF && haystack[i + j] != needle[j])
            {
                match = FALSE;
                break;
            }
        }

        if (match)
        {
            return (long)i;
        }
    }

    return -1;
}

/**
 * Validates that a found pattern location contains the expected function prologue.
 * Performs additional checks to reduce false positives.
 *
 * @param base_addr Base address of the module
 * @param rva_offset RVA offset where pattern was found
 * @param module_size Size of the module for bounds checking
 * @return TRUE if validation passes, FALSE otherwise
 */
PATTERN_STATIC BOOL validate_function_prologue(const unsigned char *base_addr, DWORD rva_offset,
                                               size_t module_size)
{
    if (rva_offset + 50 >= module_size) // Need at least 50 bytes for validation
    {
        return FALSE;
    }

    const unsigned char *func_start = base_addr + rva_offset;

    // Additional validation: check if this looks like a real function entry
    // 1. Should start with standard prologue (PUSH ECX)
    if (func_start[0] != 0x51)
    {
        return FALSE;
    }

    // 2. Validate the conditional jump targets stay inside the module.
    // JZ (0F 84) sits at offset 17, JNZ (0F 85) at offset 27 of the pattern.
    if (func_start[17] == 0x0F && func_start[18] == 0x84 &&
        !branch_target_in_bounds(func_start, 17, rva_offset, module_size))
    {
        return FALSE;
    }

    if (func_start[27] == 0x0F && func_start[28] == 0x85 &&
        !branch_target_in_bounds(func_start, 27, rva_offset, module_size))
    {
        return FALSE;
    }

    log_msg("[PATTERN] Function prologue validation passed at RVA 0x%X", rva_offset);
    return TRUE;
}

/**
 * Attempts to find the srv_gameStreamReader function using pattern matching.
 *
 * @param module_handle Handle to the loaded server.dll module
 * @param found_rva Output parameter to receive the RVA if found
 * @return PATTERN_MATCH_SUCCESS if found, appropriate error code otherwise
 */
PATTERN_MATCH_RESULT find_srv_gameStreamReader_by_pattern(HMODULE module_handle, DWORD *found_rva)
{
    if (!module_handle || !found_rva)
    {
        return PATTERN_MATCH_INVALID_PARAMS;
    }

    *found_rva = 0;

    // Get module information
    MODULEINFO module_info = {0};
    if (!GetModuleInformation(GetCurrentProcess(), module_handle, &module_info,
                              sizeof(module_info)))
    {
        log_msg("[PATTERN] Failed to get module information: %lu", GetLastError());
        return PATTERN_MATCH_MODULE_ERROR;
    }

    log_msg("[PATTERN] Searching for srv_gameStreamReader in module at %p (size: 0x%X)",
            module_info.lpBaseOfDll, module_info.SizeOfImage);

    const unsigned char *module_base = (const unsigned char *)module_info.lpBaseOfDll;
    size_t               module_size = module_info.SizeOfImage;

    // Search for the pattern
    long pattern_offset =
        find_pattern_in_memory(module_base, module_size, SRV_GAMESTREAMREADER_PATTERN,
                               SRV_GAMESTREAMREADER_MASK, SRV_GAMESTREAMREADER_PATTERN_SIZE);

    if (pattern_offset == -1)
    {
        log_msg("[PATTERN] srv_gameStreamReader pattern not found in module");
        return PATTERN_MATCH_NOT_FOUND;
    }

    DWORD rva_offset = (DWORD)pattern_offset;
    log_msg("[PATTERN] Found potential srv_gameStreamReader pattern at RVA 0x%X", rva_offset);

    // Validate the found pattern
    if (!validate_function_prologue(module_base, rva_offset, module_size))
    {
        log_msg("[PATTERN] Pattern validation failed at RVA 0x%X", rva_offset);
        return PATTERN_MATCH_VALIDATION_FAILED;
    }

    *found_rva = rva_offset;
    log_msg("[PATTERN] Successfully found srv_gameStreamReader at RVA 0x%X", rva_offset);

    return PATTERN_MATCH_SUCCESS;
}

/**
 * Converts a pattern match result to a human-readable string.
 *
 * @param result The pattern match result code
 * @return String description of the result
 */
const char *pattern_match_result_to_string(PATTERN_MATCH_RESULT result)
{
    switch (result)
    {
    case PATTERN_MATCH_SUCCESS:
        return "Success";
    case PATTERN_MATCH_NOT_FOUND:
        return "Pattern not found";
    case PATTERN_MATCH_INVALID_PARAMS:
        return "Invalid parameters";
    case PATTERN_MATCH_MODULE_ERROR:
        return "Module information error";
    case PATTERN_MATCH_VALIDATION_FAILED:
        return "Pattern validation failed";
    default:
        return "Unknown error";
    }
}