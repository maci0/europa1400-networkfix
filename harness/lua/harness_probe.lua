-- harness_probe.lua: drop in lua/ dir; runs after luaapi init
-- Writes /tmp/lua_<name>.ok when UI condition seen.  Drivers poll via lua_probe(""Network"")
local sys = system
if not sys or not sys.window_info then
  print("[lua probe] system.window_info not available")
  return
end
local function win_contains(substr)
  for _,w in ipairs(sys.window_info().windows or {}) do
    for _,v in pairs(w) do
      if type(v)=="string" and v:find(substr,1,true) then return true end
    end
  end
  return false
end
-- Example: mark Network screen visible
if win_contains("Network") then
  local f=io.open("/tmp/lua_Network.ok","w"); if f then f:write("ok\n"); f:close(); end
  print("[lua probe] lua_Network.ok written")
end
-- Extend for HostCreated / Joined similarly
if win_contains("Host") or win_contains("Create") then
  local f=io.open("/tmp/lua_HostCreated.ok","w"); if f then f:write("ok\n"); f:close(); end
end
