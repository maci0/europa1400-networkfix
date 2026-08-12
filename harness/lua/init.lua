-- Harness init.lua for luaapi.asi (europa1400-lua console).
-- Replaces the interactive REPL with a file-driven command loop so headless
-- drivers can inject input from INSIDE the game process:
--   driver writes lua source to /tmp/lua_cmd.lua (Z:\tmp\lua_cmd.lua in wine)
--   loop executes it, writes result/error to /tmp/lua_out.txt, removes cmd file
-- Input injection uses SetCursorPos + mouse_event/keybd_event, which go through
-- wineserver as hardware input, so the game's DirectInput cursor follows
-- (XTest clicks from xdotool do not reach it on submenu screens).

local ffi = require('ffi')

ffi.cdef[[
  void __stdcall Sleep(unsigned long ms);
  int  __stdcall SetCursorPos(int x, int y);
  void __stdcall mouse_event(unsigned long flags, unsigned long dx, unsigned long dy, unsigned long data, uintptr_t extra);
  void __stdcall keybd_event(unsigned char vk, unsigned char scan, unsigned long flags, uintptr_t extra);
  void* __stdcall GetConsoleWindow(void);
  int  __stdcall ShowWindow(void* hwnd, int cmd);
]]
local u32 = ffi.load('user32')
local k32 = ffi.load('kernel32')

-- Hide the AllocConsole window: it covers the game in the headless X server
local hcon = k32.GetConsoleWindow()
if hcon ~= nil then u32.ShowWindow(hcon, 0) end

local MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP = 0x0002, 0x0004
local KEYEVENTF_KEYUP = 0x0002

-- Exposed helpers for command scripts
function sleep(ms) k32.Sleep(ms) end

function click(x, y, hold_ms)
  u32.SetCursorPos(x, y)
  k32.Sleep(150)
  u32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
  k32.Sleep(hold_ms or 120)
  u32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
end

function dblclick(x, y)
  click(x, y); k32.Sleep(180); click(x, y)
end

local MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP = 0x0008, 0x0010
function rclick(x, y)
  u32.SetCursorPos(x, y)
  k32.Sleep(120)
  u32.mouse_event(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0)
  k32.Sleep(100)
  u32.mouse_event(MOUSEEVENTF_RIGHTUP, 0, 0, 0, 0)
end

-- vk: virtual-key code (0x0D Return, 0x26 Up, 0x28 Down, 0x25 Left, 0x27 Right, 0x1B Esc)
function key(vk)
  u32.keybd_event(vk, 0, 0, 0)
  k32.Sleep(80)
  u32.keybd_event(vk, 0, KEYEVENTF_KEYUP, 0)
end

function type_text(s)
  for i = 1, #s do
    local c = s:sub(i, i):upper():byte()
    key(c)
    k32.Sleep(60)
  end
end

function flag(name)
  local f = io.open('Z:\\tmp\\lua_' .. name .. '.ok', 'w')
  if f then f:write('ok\n'); f:close() end
end

local CMD, OUT = 'Z:\\tmp\\lua_cmd.lua', 'Z:\\tmp\\lua_out.txt'

local function write_out(text)
  local f = io.open(OUT, 'w')
  if f then f:write(text); f:close() end
end

print('[harness lua] command loop started (watching /tmp/lua_cmd.lua)')
flag('Ready')

while true do
  local f = io.open(CMD, 'r')
  if f then
    local src = f:read('*a')
    f:close()
    os.remove(CMD)
    local chunk, err = loadstring(src)
    if not chunk then
      write_out('COMPILE_ERROR: ' .. tostring(err))
    else
      local ok, res = pcall(chunk)
      write_out(ok and ('OK: ' .. tostring(res)) or ('RUNTIME_ERROR: ' .. tostring(res)))
    end
  end
  k32.Sleep(300)
end
