# The Guild Network Multiplayer Problem Analysis

## The Core Issue

Europa 1400: The Guild has severe multiplayer instability over VPN connections (Radmin, Hamachi, etc.). The game immediately shows "Out of Sync" errors when there are minor network fluctuations that are normal for VPN environments.

## Technical Root Cause Analysis

### 1. Non-Blocking Socket Mishandling

The game uses non-blocking sockets but handles `WSAEWOULDBLOCK` errors incorrectly:

**What should happen:**
```
recv() → WSAEWOULDBLOCK → Return 0 (no data available) → Try again later
```

**What actually happens:**
```
recv() → WSAEWOULDBLOCK → Game treats as fatal error → Out of Sync → Crash
```

The game's network code was written expecting synchronous/blocking behavior but uses non-blocking sockets, creating a fundamental mismatch.

### 2. Packet Validation Issues

**Server.dll Function at RVA 0x3720:**
This critical function (at Relative Virtual Address 0x3720 within server.dll) validates incoming network packets but has harsh error handling:
- Any checksum deviation immediately sets `context[0xE] = -1` 
- Returns negative values that propagate through the game engine
- No retry mechanism for corrupted packets
- Treats temporary network issues as permanent failures

**The packet flow problem:**
```
Network packet arrives → Minor corruption (normal in VPN) → 
Checksum fails → context[0xE] = -1 → Function returns -1 → 
Game engine interprets as fatal error → Out of Sync error
```

### 3. Send Buffer Management

The game doesn't handle partial send operations properly:
- `send()` can return partial byte counts when network buffers are full
- Game assumes all data is sent in one call
- No retry logic for `WSAEWOULDBLOCK` on send operations
- Leads to incomplete packet transmission

### 4. Timer Synchronization Assumptions

The game uses `GetTickCount()` for network synchronization:
- Assumes consistent timing across all players
- VPN latency variations break timing assumptions
- No tolerance for network-induced delays
- Timeout errors occur during normal VPN latency spikes

## Why VPNs Trigger These Issues

**Direct LAN characteristics:**
- Consistent low latency (~1ms)
- Minimal packet loss
- Predictable timing
- Large network buffers rarely fill

**VPN characteristics:**
- Variable latency (50-300ms)
- Occasional packet reordering/corruption
- Timing variations due to routing
- Smaller effective buffers due to encapsulation

The Guild's network code was designed for direct LAN play and cannot tolerate the normal behavior of VPN connections.

## Real-World Impact

### Network Environment Testing

**Direct LAN:**
- Steam version: 9+ hours stable
- Localized version: Stable but crashes after ~13 in-game years
- Issues: Periodic OOS every 1-2 turns with multiple PCs

**VPN (without fix):**
- Radmin VPN: OOS within 5 minutes, 100% failure rate
- Mixed connections (mobile hotspot + WiFi): Instant failure
- Any packet loss/latency variation: Immediate crash

**Complex game situations that amplify problems:**
- Partner building attacks during imprisonment
- Transport attacks at turn boundaries (11 PM game time)  
- Late-game complexity (years 1430+) increases crash frequency
- Simultaneous player actions

### The Solution: Targeted API Hooking

Instead of modifying server.dll directly, hook the Windows APIs it calls:

**1. recv() Hook:**
```c
// Convert WSAEWOULDBLOCK to graceful 0-byte return
if (error == WSAEWOULDBLOCK) {
    WSASetLastError(NO_ERROR);
    return 0;  // No data available, try again later
}
```

**2. send() Hook:**
```c
// Add retry logic for partial sends
while (total < len && retry_count < MAX_RETRIES) {
    sent = real_send(s, buf + total, len - total, flags);
    if (sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
        Sleep(1);  // Brief pause
        retry_count++;
        continue;  // Try again
    }
    total += sent;
}
```

**3. Server.dll Function Hook (RVA 0x3720):**
```c
// Hook the packet validation function at offset 0x3720 within server.dll
// Actual virtual address = server.dll base + 0x3720
int ret = original_function(ctx, received, totalLen);
if (ctx[0xE] < 0) ctx[0xE] = 0;  // Reset error flag
if (ret < 0) ret = 0;            // Convert failure to success
return ret;
```

**4. GetTickCount() Hook:**
```c
// Ensure consistent timer behavior
return original_GetTickCount();  // With fallback handling
```

## Why This Approach Works

**Surgical fixes:** Only affects the problematic functions, doesn't change game logic
**Non-invasive:** No modification of original game files
**Selective:** Only applies fixes to server.dll calls, not system-wide
**Proven:** 15 in-game years of stable VPN gameplay achieved

## Technical Implementation Notes

**Why not DLL replacement?**
- `ws2_32.dll` and `kernel32.dll` are "Known DLLs" - Windows always loads system versions
- Custom DLLs in game directory are ignored for security reasons
- Runtime hooking with MinHook was the only viable approach

**Caller detection:**
- Hooks only apply to calls originating from server.dll
- Uses return address checking to identify caller
- Prevents affecting other applications or system functions

## Testing Results

**Baseline (no fix):**
- VPN: 100% failure rate within minutes
- Direct LAN: Mixed results, periodic issues

**With network fix:**
- VPN: 15+ in-game years completed successfully  
- Direct LAN: Stable, no network-related crashes
- Complex scenarios: All previously failing situations now stable

The fix transforms VPN multiplayer from "completely unplayable" to "fully stable" by addressing the core incompatibility between The Guild's network assumptions and real-world VPN behavior.

## References

- [The Guild Gold Network Multiplayer Guide by Atexer](https://steamcommunity.com/sharedfiles/filedetails/?id=2918949164)
- [Original research by HarryTheBird](https://github.com/HarryTheBird/The-Guild-1-HookDLLs)
- Personal testing: 15 in-game years over Radmin VPN with mobile hotspot + WiFi setup