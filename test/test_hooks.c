/*
 * test_hooks.c: Integration tests for hook_recv / hook_send retry logic.
 *
 * Built only with -DNETWORKFIX_TEST. Drives the real hook functions from
 * src/hooks.c with scripted mock recv/send replacements. Verifies the
 * WSAEWOULDBLOCK retry/conversion behavior end-to-end without needing a
 * running game or MinHook trampolines.
 */

#define WIN32_LEAN_AND_MEAN
#include "hooks.h"
#include "pattern_matcher.h"
#include "versions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winsock2.h>
#include <psapi.h>

/* hooks.c globals exposed under NETWORKFIX_TEST */
extern int(WSAAPI *real_recv)(SOCKET, char *, int, int);
extern int(WSAAPI *real_send)(SOCKET, const char *, int, int);

typedef int(__cdecl *srv_gameStreamReader_t)(int *ctx, int received, int totalLen);
extern srv_gameStreamReader_t real_srv_gameStreamReader;
int __cdecl                   hook_srv_gameStreamReader(int *ctx, int received, int totalLen);

/* pattern_matcher.c internals exposed under NETWORKFIX_TEST */
long find_pattern_in_memory(const unsigned char *haystack, size_t haystack_size, const unsigned char *needle,
                            const unsigned char *mask, size_t needle_size);
BOOL validate_function_prologue(const unsigned char *base_addr, DWORD rva_offset, size_t module_size);
/* sha256.c public API */
BOOL calculate_file_sha256(const wchar_t *filepath, char *hash_output, size_t output_size);

/* main.c global referenced by hooks.c (get_server_path_from_ini). Tests never
 * touch that codepath, but the symbol must resolve at link time. */
HMODULE g_hModule = NULL;

/* ---- Sleep counter (replaces Sleep() inside hook_send retry loop) ---- */
static int g_sleep_calls = 0;
static int g_sleep_total_ms = 0;
void       test_sleep(DWORD ms)
{
    g_sleep_calls++;
    g_sleep_total_ms += (int)ms;
}

/* ---- Scriptable recv mock ---- */
typedef struct
{
    int        block_count;     /* number of WSAEWOULDBLOCK errors before success */
    int        return_value;    /* value to return on the success call */
    int        final_error;     /* non-zero -> on success call return SOCKET_ERROR + this WSA error */
    const char *payload;        /* data to copy into recv buffer on success (NULL = none) */
    int        call_count;
} recv_script;

static recv_script g_recv_script;

static int WSAAPI mock_recv(SOCKET s, char *buf, int len, int flags)
{
    (void)s;
    (void)flags;
    g_recv_script.call_count++;

    if (g_recv_script.call_count <= g_recv_script.block_count)
    {
        WSASetLastError(WSAEWOULDBLOCK);
        return SOCKET_ERROR;
    }

    if (g_recv_script.final_error != 0)
    {
        WSASetLastError(g_recv_script.final_error);
        return SOCKET_ERROR;
    }

    if (g_recv_script.payload && len > 0)
    {
        int n = (int)strlen(g_recv_script.payload);
        if (n > len)
            n = len;
        memcpy(buf, g_recv_script.payload, n);
        return n;
    }

    return g_recv_script.return_value;
}

/* ---- Scriptable send mock ---- */
typedef struct
{
    int  chunk_size;    /* bytes to accept per successful send call (<=0 -> all remaining) */
    int  block_count;   /* WSAEWOULDBLOCK errors before each successful chunk */
    int  abort_after;   /* bytes after which to inject abort_error (-1 disabled) */
    int  abort_error;   /* WSA error to inject (e.g. WSAECONNRESET) */
    int  zero_at;       /* total bytes after which to return 0 ("connection closed") (-1 disabled) */
    int  call_count;
    int  total_accepted;
    int  block_streak;  /* internal: blocks emitted in current streak */
} send_script;

static send_script g_send_script;

static int WSAAPI mock_send(SOCKET s, const char *buf, int len, int flags)
{
    (void)s;
    (void)buf;
    (void)flags;
    g_send_script.call_count++;

    if (g_send_script.block_streak < g_send_script.block_count)
    {
        g_send_script.block_streak++;
        WSASetLastError(WSAEWOULDBLOCK);
        return SOCKET_ERROR;
    }
    g_send_script.block_streak = 0;

    if (g_send_script.abort_after >= 0 && g_send_script.total_accepted >= g_send_script.abort_after)
    {
        WSASetLastError(g_send_script.abort_error);
        return SOCKET_ERROR;
    }

    if (g_send_script.zero_at >= 0 && g_send_script.total_accepted >= g_send_script.zero_at)
    {
        return 0;
    }

    int chunk = g_send_script.chunk_size > 0 ? g_send_script.chunk_size : len;
    if (chunk > len)
        chunk = len;
    g_send_script.total_accepted += chunk;
    return chunk;
}

/* ---- Helpers ---- */
static void reset_state(void)
{
    memset(&g_recv_script, 0, sizeof(g_recv_script));
    memset(&g_send_script, 0, sizeof(g_send_script));
    g_send_script.abort_after = -1;
    g_send_script.zero_at = -1;
    g_sleep_calls = 0;
    g_sleep_total_ms = 0;
    real_recv = mock_recv;
    real_send = mock_send;
    WSASetLastError(0);
}

static int g_failures = 0;
#define CHECK(cond, ...)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            fprintf(stderr, "  FAIL %s:%d: " #cond "\n    ", __FILE__, __LINE__);                                      \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fprintf(stderr, "\n");                                                                                     \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

#define RUN(name)                                                                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        printf("[test] %s\n", #name);                                                                                  \
        reset_state();                                                                                                 \
        name();                                                                                                        \
    } while (0)

/* ---- Tests ---- */

/* recv: WSAEWOULDBLOCK is converted to a 0-byte read and the last error is cleared. */
static void test_recv_wouldblock_returns_zero(void)
{
    g_recv_script.block_count = 1;
    g_recv_script.return_value = 0;

    char buf[64];
    int  r = hook_recv((SOCKET)1, buf, sizeof(buf), 0);

    CHECK(r == 0, "expected 0, got %d", r);
    CHECK(WSAGetLastError() == 0, "expected last error cleared, got %d", WSAGetLastError());
    CHECK(g_recv_script.call_count == 1, "expected 1 real_recv call, got %d", g_recv_script.call_count);
}

/* recv: normal data flows through unchanged. */
static void test_recv_passes_data_through(void)
{
    g_recv_script.payload = "hello";

    char buf[64] = {0};
    int  r = hook_recv((SOCKET)1, buf, sizeof(buf), 0);

    CHECK(r == 5, "expected 5 bytes, got %d", r);
    CHECK(memcmp(buf, "hello", 5) == 0, "buffer contents wrong");
}

/* recv: non-wouldblock errors propagate unchanged. */
static void test_recv_propagates_other_errors(void)
{
    g_recv_script.final_error = WSAECONNRESET;

    char buf[64];
    int  r = hook_recv((SOCKET)1, buf, sizeof(buf), 0);

    CHECK(r == SOCKET_ERROR, "expected SOCKET_ERROR, got %d", r);
}

/* send: send buffer full N times, then drains. All bytes sent, retries logged. */
static void test_send_retries_then_succeeds(void)
{
    const char *msg = "abcdefghij"; /* 10 bytes */
    g_send_script.block_count = 3;  /* 3 wouldblocks, then full success */
    g_send_script.chunk_size = 0;   /* accept all on success */

    int r = hook_send((SOCKET)1, msg, 10, 0);

    CHECK(r == 10, "expected all 10 bytes, got %d", r);
    CHECK(g_sleep_calls == 3, "expected 3 sleeps, got %d", g_sleep_calls);
    CHECK(g_send_script.total_accepted == 10, "expected 10 bytes accepted, got %d", g_send_script.total_accepted);
}

/* send: short writes loop until full payload is delivered. */
static void test_send_handles_partial_sends(void)
{
    const char *msg = "abcdefghij"; /* 10 bytes */
    g_send_script.chunk_size = 3;   /* 3+3+3+1 */

    int r = hook_send((SOCKET)1, msg, 10, 0);

    CHECK(r == 10, "expected 10 bytes, got %d", r);
    CHECK(g_send_script.call_count == 4, "expected 4 send calls, got %d", g_send_script.call_count);
    CHECK(g_sleep_calls == 0, "expected no sleeps on partial sends, got %d", g_sleep_calls);
}

/* send: WSAECONNRESET after partial progress returns the partial total, not SOCKET_ERROR. */
static void test_send_connreset_returns_partial(void)
{
    const char *msg = "abcdefghij";
    g_send_script.chunk_size = 4;
    g_send_script.abort_after = 4;
    g_send_script.abort_error = WSAECONNRESET;

    int r = hook_send((SOCKET)1, msg, 10, 0);

    CHECK(r == 4, "expected partial 4, got %d", r);
    CHECK(WSAGetLastError() == WSAECONNRESET, "expected WSAECONNRESET preserved, got %d", WSAGetLastError());
}

/* send: WSAECONNABORTED with zero progress returns SOCKET_ERROR. */
static void test_send_connaborted_zero_progress(void)
{
    const char *msg = "abcdefghij";
    g_send_script.abort_after = 0;
    g_send_script.abort_error = WSAECONNABORTED;

    int r = hook_send((SOCKET)1, msg, 10, 0);

    CHECK(r == SOCKET_ERROR, "expected SOCKET_ERROR on zero-progress abort, got %d", r);
}

/* send: peer-closed (return 0) bails out and returns whatever was already sent. */
static void test_send_zero_indicates_closed(void)
{
    const char *msg = "abcdefghij";
    g_send_script.chunk_size = 3;
    g_send_script.zero_at = 6;

    int r = hook_send((SOCKET)1, msg, 10, 0);

    CHECK(r == 6, "expected 6 bytes before close, got %d", r);
}

/* send: retry counter resets after a successful chunk, so an interleaved
 * pattern (block, send, block, send, ...) never accumulates toward max retries. */
static void test_send_retry_counter_resets(void)
{
    const char *msg = "abcd";
    g_send_script.chunk_size = 1;
    g_send_script.block_count = 2; /* 2 blocks ahead of every successful chunk */

    int r = hook_send((SOCKET)1, msg, 4, 0);

    CHECK(r == 4, "expected 4 bytes, got %d", r);
    CHECK(g_sleep_calls == 8, "expected 8 sleeps (2 per chunk x 4), got %d", g_sleep_calls);
}

/* ---- srv_gameStreamReader mock + tests ---- */
static int g_srv_call_count;
static int g_srv_return;
static int g_srv_set_ctx_e; /* if non-zero, mock writes this into ctx[0xE] */

static int __cdecl mock_srv(int *ctx, int received, int totalLen)
{
    (void)received;
    (void)totalLen;
    g_srv_call_count++;
    if (g_srv_set_ctx_e != 0)
    {
        ctx[0xE] = g_srv_set_ctx_e;
    }
    return g_srv_return;
}

static void reset_srv_state(void)
{
    g_srv_call_count = 0;
    g_srv_return = 0;
    g_srv_set_ctx_e = 0;
    real_srv_gameStreamReader = mock_srv;
}

static void test_srv_null_ctx_returns_minus_one(void)
{
    reset_srv_state();
    int r = hook_srv_gameStreamReader(NULL, 100, 200);
    CHECK(r == -1, "expected -1 on NULL ctx, got %d", r);
    CHECK(g_srv_call_count == 0, "expected mock not called, was called %d times", g_srv_call_count);
}

static void test_srv_negative_ctx_e_is_zeroed(void)
{
    reset_srv_state();
    g_srv_return = 42;
    g_srv_set_ctx_e = -7;

    int ctx[32] = {0};
    int r = hook_srv_gameStreamReader(ctx, 100, 200);

    CHECK(r == 42, "expected return 42, got %d", r);
    CHECK(ctx[0xE] == 0, "expected ctx[0xE] zeroed, got %d", ctx[0xE]);
}

static void test_srv_negative_return_is_zeroed(void)
{
    reset_srv_state();
    g_srv_return = -5;

    int ctx[32] = {0};
    int r = hook_srv_gameStreamReader(ctx, 100, 200);

    CHECK(r == 0, "expected return 0 (was -5), got %d", r);
}

static void test_srv_clean_passes_through(void)
{
    reset_srv_state();
    g_srv_return = 123;

    int ctx[32] = {0};
    ctx[0xE] = 50;
    int r = hook_srv_gameStreamReader(ctx, 100, 200);

    CHECK(r == 123, "expected return 123, got %d", r);
    CHECK(ctx[0xE] == 50, "expected ctx[0xE] untouched, got %d", ctx[0xE]);
}

/* ---- pattern matcher tests ---- */
static void test_pattern_finds_exact_match(void)
{
    const unsigned char hay[] = {0xDE, 0xAD, 0x51, 0x8B, 0x4C, 0x24, 0xBE, 0xEF};
    const unsigned char needle[] = {0x51, 0x8B, 0x4C, 0x24};
    const unsigned char mask[] = {0xFF, 0xFF, 0xFF, 0xFF};

    long off = find_pattern_in_memory(hay, sizeof(hay), needle, mask, sizeof(needle));
    CHECK(off == 2, "expected offset 2, got %ld", off);
}

static void test_pattern_returns_minus_one_when_absent(void)
{
    const unsigned char hay[] = {0xDE, 0xAD, 0xBE, 0xEF};
    const unsigned char needle[] = {0x51, 0x8B};
    const unsigned char mask[] = {0xFF, 0xFF};

    long off = find_pattern_in_memory(hay, sizeof(hay), needle, mask, sizeof(needle));
    CHECK(off == -1, "expected -1, got %ld", off);
}

static void test_pattern_mask_ignores_wildcards(void)
{
    /* Haystack differs in the wildcard byte (offset 1 of match), exact bytes match. */
    const unsigned char hay[] = {0x00, 0x51, 0xAB, 0x4C, 0x24, 0x00};
    const unsigned char needle[] = {0x51, 0x00, 0x4C, 0x24}; /* needle[1] is wildcard */
    const unsigned char mask[] = {0xFF, 0x00, 0xFF, 0xFF};

    long off = find_pattern_in_memory(hay, sizeof(hay), needle, mask, sizeof(needle));
    CHECK(off == 1, "expected offset 1 with wildcard, got %ld", off);
}

static void test_pattern_rejects_when_haystack_too_small(void)
{
    const unsigned char hay[] = {0x51};
    const unsigned char needle[] = {0x51, 0x8B};
    const unsigned char mask[] = {0xFF, 0xFF};

    long off = find_pattern_in_memory(hay, sizeof(hay), needle, mask, sizeof(needle));
    CHECK(off == -1, "expected -1 on undersized haystack, got %ld", off);
}

static void test_pattern_rejects_null_args(void)
{
    const unsigned char needle[] = {0x51};
    const unsigned char mask[] = {0xFF};

    CHECK(find_pattern_in_memory(NULL, 10, needle, mask, 1) == -1, "expected -1 on NULL haystack");
    CHECK(find_pattern_in_memory(needle, 10, NULL, mask, 1) == -1, "expected -1 on NULL needle");
    CHECK(find_pattern_in_memory(needle, 10, needle, NULL, 1) == -1, "expected -1 on NULL mask");
    CHECK(find_pattern_in_memory(needle, 10, needle, mask, 0) == -1, "expected -1 on zero needle_size");
}

/* ---- validate_function_prologue tests ---- */

/* Build a synthetic function prologue blob.
 * offsets 0..17 prologue bytes, [18..23] JZ, [24..27] body, [28..33] JNZ,
 * remaining bytes filler. JZ/JNZ relative offsets are 32-bit signed. */
static void build_valid_prologue(unsigned char *blob, size_t blob_size, int32_t jz_rel, int32_t jnz_rel)
{
    memset(blob, 0x90, blob_size); /* NOP filler */
    blob[0] = 0x51;                /* PUSH ECX */
    blob[18] = 0x0F;
    blob[19] = 0x84;
    memcpy(blob + 20, &jz_rel, sizeof(jz_rel));
    blob[28] = 0x0F;
    blob[29] = 0x85;
    memcpy(blob + 30, &jnz_rel, sizeof(jnz_rel));
}

static void test_validate_accepts_in_bounds_prologue(void)
{
    unsigned char blob[128];
    build_valid_prologue(blob, sizeof(blob), 10, 5); /* targets land inside blob */
    BOOL ok = validate_function_prologue(blob, 0, sizeof(blob));
    CHECK(ok == TRUE, "expected TRUE, got %d", ok);
}

static void test_validate_rejects_missing_push_ecx(void)
{
    unsigned char blob[128];
    build_valid_prologue(blob, sizeof(blob), 10, 5);
    blob[0] = 0x90; /* not PUSH ECX */
    BOOL ok = validate_function_prologue(blob, 0, sizeof(blob));
    CHECK(ok == FALSE, "expected FALSE on missing PUSH ECX, got %d", ok);
}

static void test_validate_rejects_jz_out_of_bounds(void)
{
    unsigned char blob[128];
    /* huge positive offset -> jz_target = 24 + huge >= module_size */
    build_valid_prologue(blob, sizeof(blob), 0x7FFFFFFF, 5);
    BOOL ok = validate_function_prologue(blob, 0, sizeof(blob));
    CHECK(ok == FALSE, "expected FALSE on out-of-bounds JZ, got %d", ok);
}

static void test_validate_rejects_jnz_out_of_bounds(void)
{
    unsigned char blob[128];
    build_valid_prologue(blob, sizeof(blob), 5, 0x7FFFFFFF);
    BOOL ok = validate_function_prologue(blob, 0, sizeof(blob));
    CHECK(ok == FALSE, "expected FALSE on out-of-bounds JNZ, got %d", ok);
}

static void test_validate_rejects_when_too_close_to_end(void)
{
    unsigned char blob[128];
    build_valid_prologue(blob, sizeof(blob), 5, 5);
    /* rva_offset such that rva_offset + 50 >= module_size */
    BOOL ok = validate_function_prologue(blob, 90, sizeof(blob));
    CHECK(ok == FALSE, "expected FALSE when fewer than 50 bytes remain, got %d", ok);
}

/* ---- SHA256 tests ---- */

static BOOL write_temp_file(const wchar_t *path, const void *data, DWORD size)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    DWORD written = 0;
    BOOL  ok = WriteFile(h, data, size, &written, NULL) && written == size;
    CloseHandle(h);
    return ok;
}

static BOOL is_lowercase_hex_64(const char *s)
{
    for (int i = 0; i < 64; i++)
    {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return FALSE;
    }
    return s[64] == '\0';
}

/* Round-trip: same input -> same hash, different input -> different hash. */
static void test_sha256_deterministic_and_collision_free_for_distinct_inputs(void)
{
    const wchar_t *p1 = L"test_sha_a.bin";
    const wchar_t *p2 = L"test_sha_b.bin";
    CHECK(write_temp_file(p1, "hello world", 11) == TRUE, "could not write temp file p1");
    CHECK(write_temp_file(p2, "hello world!", 12) == TRUE, "could not write temp file p2");

    char h1[65] = {0}, h1_again[65] = {0}, h2[65] = {0};
    CHECK(calculate_file_sha256(p1, h1, sizeof(h1)) == TRUE, "hash p1 failed");
    CHECK(calculate_file_sha256(p1, h1_again, sizeof(h1_again)) == TRUE, "hash p1 again failed");
    CHECK(calculate_file_sha256(p2, h2, sizeof(h2)) == TRUE, "hash p2 failed");

    CHECK(is_lowercase_hex_64(h1), "h1 not 64 lowercase hex chars: %s", h1);
    CHECK(strcmp(h1, h1_again) == 0, "deterministic check failed: %s vs %s", h1, h1_again);
    CHECK(strcmp(h1, h2) != 0, "different inputs produced same hash: %s", h1);

    DeleteFileW(p1);
    DeleteFileW(p2);
}

static void test_sha256_empty_file(void)
{
    const wchar_t *path = L"test_sha_empty.bin";
    CHECK(write_temp_file(path, "", 0) == TRUE, "could not write temp file");

    char hash[65] = {0};
    BOOL ok = calculate_file_sha256(path, hash, sizeof(hash));
    CHECK(ok == TRUE, "calculate_file_sha256 failed");
    CHECK(is_lowercase_hex_64(hash), "hash not 64 lowercase hex chars: %s", hash);

    DeleteFileW(path);
}

static void test_sha256_missing_file_returns_false(void)
{
    char hash[65] = {0};
    BOOL ok = calculate_file_sha256(L"does_not_exist_xyz.bin", hash, sizeof(hash));
    CHECK(ok == FALSE, "expected FALSE on missing file, got TRUE");
}

static void test_sha256_undersized_buffer_returns_false(void)
{
    const wchar_t *path = L"test_sha_small_buf.bin";
    CHECK(write_temp_file(path, "abc", 3) == TRUE, "could not write temp file");

    char hash[10] = {0}; /* too small for 64-char hex + NUL */
    BOOL ok = calculate_file_sha256(path, hash, sizeof(hash));
    CHECK(ok == FALSE, "expected FALSE on undersized buffer, got TRUE");

    DeleteFileW(path);
}

/* ---- get_server_path_from_ini tests ---- */

/* Writes a game.ini next to the running .exe (where the function looks). Returns
 * the absolute ini path so the test can delete it after. */
static BOOL write_ini_next_to_exe(const char *contents, char *out_path, size_t out_path_size)
{
    char exe_path[MAX_PATH];
    if (GetModuleFileNameA(NULL, exe_path, sizeof(exe_path)) == 0)
        return FALSE;

    char *last_sep = strrchr(exe_path, '\\');
    if (!last_sep)
        last_sep = strrchr(exe_path, '/');
    if (!last_sep)
        return FALSE;
    *(last_sep + 1) = '\0';

    int n = snprintf(out_path, out_path_size, "%sgame.ini", exe_path);
    if (n < 0 || (size_t)n >= out_path_size)
        return FALSE;

    HANDLE h = CreateFileA(out_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    DWORD written = 0;
    DWORD len = (DWORD)strlen(contents);
    BOOL  ok = WriteFile(h, contents, len, &written, NULL) && written == len;
    CloseHandle(h);
    return ok;
}

static void test_ini_returns_unquoted_path(void)
{
    char ini_path[MAX_PATH];
    CHECK(write_ini_next_to_exe("[Network]\r\nServer=foo\\bar\\server.dll\r\n", ini_path, sizeof(ini_path)) == TRUE,
          "could not write game.ini");

    const char *p = get_server_path_from_ini(GetModuleHandleA(NULL));
    CHECK(p != NULL, "expected non-NULL");
    if (p)
    {
        CHECK(strcmp(p, "foo\\bar\\server.dll") == 0, "got: %s", p);
    }

    DeleteFileA(ini_path);
}

static void test_ini_strips_surrounding_quotes(void)
{
    char ini_path[MAX_PATH];
    CHECK(write_ini_next_to_exe("[Network]\r\nServer=\"C:\\path with space\\server.dll\"\r\n", ini_path,
                                sizeof(ini_path)) == TRUE,
          "could not write game.ini");

    const char *p = get_server_path_from_ini(GetModuleHandleA(NULL));
    CHECK(p != NULL, "expected non-NULL");
    if (p)
    {
        CHECK(strcmp(p, "C:\\path with space\\server.dll") == 0, "got: %s", p);
    }

    DeleteFileA(ini_path);
}

static void test_ini_missing_key_returns_null(void)
{
    char ini_path[MAX_PATH];
    CHECK(write_ini_next_to_exe("[Other]\r\nKey=value\r\n", ini_path, sizeof(ini_path)) == TRUE,
          "could not write game.ini");

    const char *p = get_server_path_from_ini(GetModuleHandleA(NULL));
    CHECK(p == NULL, "expected NULL, got: %s", p ? p : "(null)");

    DeleteFileA(ini_path);
}

static void test_ini_missing_file_returns_null(void)
{
    /* Make sure no game.ini exists next to the exe */
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));
    char *sep = strrchr(exe_path, '\\');
    if (!sep)
        sep = strrchr(exe_path, '/');
    if (sep)
    {
        *(sep + 1) = '\0';
        char ini_path[MAX_PATH];
        snprintf(ini_path, sizeof(ini_path), "%sgame.ini", exe_path);
        DeleteFileA(ini_path);
    }

    const char *p = get_server_path_from_ini(GetModuleHandleA(NULL));
    CHECK(p == NULL, "expected NULL, got: %s", p ? p : "(null)");
}

static void test_ini_null_module_returns_null(void)
{
    const char *p = get_server_path_from_ini(NULL);
    CHECK(p == NULL, "expected NULL on NULL hModule, got: %s", p ? p : "(null)");
}

/* ---- Real server.dll fixture tests ---- */

/* Looks up a hash in known_versions[]. Returns matching entry or NULL. */
static const server_version_info_t *lookup_known_version(const char *hash)
{
    for (int i = 0; known_versions[i].sha256_hash != NULL; i++)
    {
        if (strcmp(hash, known_versions[i].sha256_hash) == 0)
            return &known_versions[i];
    }
    return NULL;
}

/* Runs all four real-DLL checks against a single fixture: hash matches a
 * known_versions[] entry, pattern matcher reports the expected RVA, prologue
 * heuristic accepts the real bytes at that RVA, and the pattern occurs
 * exactly once in the loaded image. */
static void run_fixture_tests(const wchar_t *fixture_path)
{
    char fixture_path_a[MAX_PATH] = {0};
    WideCharToMultiByte(CP_ACP, 0, fixture_path, -1, fixture_path_a, sizeof(fixture_path_a), NULL, NULL);
    printf("  fixture: %s\n", fixture_path_a);

    char hash[65] = {0};
    CHECK(calculate_file_sha256(fixture_path, hash, sizeof(hash)) == TRUE, "hash failed");
    CHECK(is_lowercase_hex_64(hash), "hash not 64 lowercase hex chars: %s", hash);

    const server_version_info_t *v = lookup_known_version(hash);
    CHECK(v != NULL, "hash %s does not match any known_versions[] entry", hash);
    if (!v)
        return;
    printf("    matched: %s (expected RVA 0x%X)\n", v->version_name, (unsigned)v->target_rva);

    HMODULE h = LoadLibraryW(fixture_path);
    CHECK(h != NULL, "LoadLibraryW failed: %lu", GetLastError());
    if (!h)
        return;

    /* Pattern matcher returns the expected RVA for this version. */
    DWORD                rva = 0;
    PATTERN_MATCH_RESULT r = find_srv_gameStreamReader_by_pattern(h, &rva);
    CHECK(r == PATTERN_MATCH_SUCCESS, "pattern matcher returned %d (%s)", (int)r,
          pattern_match_result_to_string(r));
    CHECK(rva == v->target_rva, "RVA mismatch: got 0x%X, expected 0x%X", (unsigned)rva, (unsigned)v->target_rva);

    /* Prologue heuristic accepts the real bytes at the known RVA. */
    MODULEINFO mi = {0};
    CHECK(GetModuleInformation(GetCurrentProcess(), h, &mi, sizeof(mi)) != 0,
          "GetModuleInformation failed: %lu", GetLastError());
    if (mi.SizeOfImage > 0)
    {
        BOOL ok = validate_function_prologue((const unsigned char *)mi.lpBaseOfDll, v->target_rva, mi.SizeOfImage);
        CHECK(ok == TRUE, "prologue validation failed at RVA 0x%X", (unsigned)v->target_rva);
    }

    /* Uniqueness: pattern occurs exactly once. */
    const unsigned char needle[] = {
        0x51, 0x8B, 0x4C, 0x24, 0x0C, 0x53, 0x55, 0x8B, 0x6C, 0x24, 0x10, 0x56, 0x57,
        0x85, 0xED, 0x8B, 0xF1, 0x0F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x80, 0x7D, 0x5C,
        0x72, 0x0F, 0x85, 0x00, 0x00, 0x00, 0x00, 0x8B, 0x45, 0x38,
    };
    const unsigned char mask[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF,
    };
    if (mi.SizeOfImage > 0)
    {
        const unsigned char *base = (const unsigned char *)mi.lpBaseOfDll;
        size_t               size = mi.SizeOfImage;
        long                 first = find_pattern_in_memory(base, size, needle, mask, sizeof(needle));
        CHECK(first >= 0, "first match not found");
        if (first >= 0)
        {
            long step = first + 1;
            long second = find_pattern_in_memory(base + step, size - step, needle, mask, sizeof(needle));
            CHECK(second == -1, "pattern matched more than once (second hit at +0x%lX)", first + 1 + second);
        }
    }

    FreeLibrary(h);
}

/* Enumerates server*.dll in repo root and runs the fixture tests for each.
 * This lets us cover the Steam build, the GOG build, and any future variant
 * simply by dropping the file next to the test binary. */
static void test_real_server_dll_fixtures(void)
{
    WIN32_FIND_DATAW fd;
    HANDLE           hf = FindFirstFileW(L"server*.dll", &fd);
    if (hf == INVALID_HANDLE_VALUE)
    {
        printf("  SKIP (no server*.dll fixtures)\n");
        return;
    }

    int seen = 0;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        run_fixture_tests(fd.cFileName);
        seen++;
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);

    if (seen == 0)
        printf("  SKIP (no server*.dll fixtures)\n");
}

/* Negative control: a totally unrelated system DLL must not match the
 * srv_gameStreamReader pattern. Proves the pattern is specific. */
static void test_pattern_matcher_does_not_match_unrelated_dll(void)
{
    HMODULE h = LoadLibraryW(L"ntdll.dll");
    CHECK(h != NULL, "LoadLibraryW(ntdll) failed: %lu", GetLastError());
    if (!h)
        return;

    DWORD                rva = 0xDEADBEEF;
    PATTERN_MATCH_RESULT r = find_srv_gameStreamReader_by_pattern(h, &rva);
    CHECK(r != PATTERN_MATCH_SUCCESS, "unexpected match in ntdll at RVA 0x%X", (unsigned)rva);
    CHECK(rva == 0, "rva should be zeroed on failure, got 0x%X", (unsigned)rva);

    /* Do NOT FreeLibrary(ntdll): handle is shared system-wide. */
}

int main(void)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        fprintf(stderr, "WSAStartup failed: %d\n", WSAGetLastError());
        return 1;
    }

    RUN(test_recv_wouldblock_returns_zero);
    RUN(test_recv_passes_data_through);
    RUN(test_recv_propagates_other_errors);
    RUN(test_send_retries_then_succeeds);
    RUN(test_send_handles_partial_sends);
    RUN(test_send_connreset_returns_partial);
    RUN(test_send_connaborted_zero_progress);
    RUN(test_send_zero_indicates_closed);
    RUN(test_send_retry_counter_resets);

    RUN(test_srv_null_ctx_returns_minus_one);
    RUN(test_srv_negative_ctx_e_is_zeroed);
    RUN(test_srv_negative_return_is_zeroed);
    RUN(test_srv_clean_passes_through);

    printf("[test] test_pattern_finds_exact_match\n");
    test_pattern_finds_exact_match();
    printf("[test] test_pattern_returns_minus_one_when_absent\n");
    test_pattern_returns_minus_one_when_absent();
    printf("[test] test_pattern_mask_ignores_wildcards\n");
    test_pattern_mask_ignores_wildcards();
    printf("[test] test_pattern_rejects_when_haystack_too_small\n");
    test_pattern_rejects_when_haystack_too_small();
    printf("[test] test_pattern_rejects_null_args\n");
    test_pattern_rejects_null_args();

    printf("[test] test_validate_accepts_in_bounds_prologue\n");
    test_validate_accepts_in_bounds_prologue();
    printf("[test] test_validate_rejects_missing_push_ecx\n");
    test_validate_rejects_missing_push_ecx();
    printf("[test] test_validate_rejects_jz_out_of_bounds\n");
    test_validate_rejects_jz_out_of_bounds();
    printf("[test] test_validate_rejects_jnz_out_of_bounds\n");
    test_validate_rejects_jnz_out_of_bounds();
    printf("[test] test_validate_rejects_when_too_close_to_end\n");
    test_validate_rejects_when_too_close_to_end();

    printf("[test] test_sha256_deterministic_and_collision_free_for_distinct_inputs\n");
    test_sha256_deterministic_and_collision_free_for_distinct_inputs();
    printf("[test] test_sha256_empty_file\n");
    test_sha256_empty_file();
    printf("[test] test_sha256_missing_file_returns_false\n");
    test_sha256_missing_file_returns_false();
    printf("[test] test_sha256_undersized_buffer_returns_false\n");
    test_sha256_undersized_buffer_returns_false();
    printf("[test] test_real_server_dll_fixtures\n");
    test_real_server_dll_fixtures();
    printf("[test] test_pattern_matcher_does_not_match_unrelated_dll\n");
    test_pattern_matcher_does_not_match_unrelated_dll();

    printf("[test] test_ini_returns_unquoted_path\n");
    test_ini_returns_unquoted_path();
    printf("[test] test_ini_strips_surrounding_quotes\n");
    test_ini_strips_surrounding_quotes();
    printf("[test] test_ini_missing_key_returns_null\n");
    test_ini_missing_key_returns_null();
    printf("[test] test_ini_missing_file_returns_null\n");
    test_ini_missing_file_returns_null();
    printf("[test] test_ini_null_module_returns_null\n");
    test_ini_null_module_returns_null();

    WSACleanup();

    if (g_failures == 0)
    {
        printf("\nAll tests passed.\n");
        return 0;
    }
    printf("\n%d test assertion(s) failed.\n", g_failures);
    return 1;
}
