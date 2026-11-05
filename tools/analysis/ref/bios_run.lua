-- Reference run: boot the real BS-X BIOS, launch the pack through the BIOS's own
-- map and exec routines (no town), then log what the game touches and take
-- periodic screenshots. Output goes to PVS_REF_DIR, which must be under the
-- git-ignored local/ folder because it holds ROM-derived data.
--
-- Env: PVS_REF_DIR (required), PVS_LAUNCH_FRAME (default 240),
--      PVS_SHOT_EVERY frames (default 600), PVS_STOP_FRAME (default none).
local dir = assert(os.getenv("PVS_REF_DIR"), "set PVS_REF_DIR")
local launch_frame = tonumber(os.getenv("PVS_LAUNCH_FRAME") or "240")
local shot_every = tonumber(os.getenv("PVS_SHOT_EVERY") or "600")
local stop_frame = tonumber(os.getenv("PVS_STOP_FRAME") or "0")
local shot_at = os.getenv("PVS_SHOT_AT")
local press_start_at = tonumber(os.getenv("PVS_PRESS_START_AT") or "0")

local logf = assert(io.open(dir .. "/log.txt", "w"))
local function log(s) logf:write(s, "\n"); logf:flush() end

local mem = emu.memType.snesMemory
local frame, launched_at, entered = 0, nil, false

local function pc24()
  local s = emu.getState()
  return ((s["cpu.k"] or 0) << 16) | (s["cpu.pc"] or 0)
end

-- First access per target, plus a count, for anything outside the pack.
local seen = {}
local function note(kind, addr, value)
  if not entered then return end
  -- Font and save reads touch many addresses, so group those by 4 KB page.
  local grouped = (kind == "read-bios" or kind == "read-sram" or kind == "write-sram")
  local key = string.format("%s %06X", kind, grouped and (addr & 0xFFF000) or addr)
  local e = seen[key]
  if not e then
    e = { n = 0 }
    seen[key] = e
    log(string.format("first %s $%06X = %02X at PC $%06X frame %d", kind, addr, value or 0, pc24(), frame))
  end
  e.n = e.n + 1
end

local cb = emu.callbackType
local function watch(kind, ctype, lo, hi)
  emu.addMemoryCallback(function(addr, value) note(kind, addr, value) end, ctype, lo, hi, emu.cpuType.snes, mem)
end
-- Registered only once the pack starts: Lua on every read during the BIOS's own
-- boot slows the run to about 30 fps.
local function start_watching()
  watch("read-base", cb.read, 0x002188, 0x00219F)
  watch("write-base", cb.write, 0x002188, 0x00219F)
  watch("read-bios", cb.read, 0x808000, 0x8FFFFF)
  watch("read-sram", cb.read, 0x105000, 0x175FFF)
  watch("write-sram", cb.write, 0x105000, 0x175FFF)
  watch("write-mcc", cb.write, 0x005000, 0x0F5FFF)
  watch("exec-sram", cb.exec, 0x105000, 0x175FFF)
  -- Every change to the SoundLink output bit, with the clock: the audio timeline.
  local last = -1
  emu.addMemoryCallback(function(_, value)
    if value ~= last then
      last = value
      log(string.format("soundlink $2197=%02X at clock %02d:%02d frame +%d PC $%06X",
        value, emu.read(0x7E4183, mem), emu.read(0x7E4184, mem), frame - launched_at, pc24()))
    end
  end, cb.write, 0x002197, 0x002197, emu.cpuType.snes, mem)
end

local function dump(name, memtype, size)
  local f = assert(io.open(dir .. "/" .. name, "wb"))
  local buf = {}
  for i = 0, size - 1 do
    buf[#buf + 1] = string.char(emu.read(i, memtype))
    if #buf == 4096 then f:write(table.concat(buf)); buf = {} end
  end
  f:write(table.concat(buf)); f:close()
end

-- Runs from WRAM once the BIOS has installed its SRAM calls. Does what the BIOS's map ($80:BC2C) and exec ($80:BC84) routines
-- do, without calling them: those run inside the BIOS's IRQ-driven threads.
local STUB = 0x1E00
local NMI_HOOK = 0x105978
local function mcc(reg, value)  -- LDA #value / STA $reg:5000
  return { 0xA9, value, 0x8F, 0x00, 0x50, reg }
end
local stub = { 0x78, 0xE2, 0x20, 0xC2, 0x10,           -- SEI, 8-bit A, 16-bit X
               0xA9, 0x80, 0x48, 0xAB,                 -- data bank $80
               0x9C, 0x00, 0x42,                       -- STZ $4200
               0xA9, 0xFE, 0xA2, 0xAA, 0xBB,           -- return the SPC to its IPL
               0x8D, 0x40, 0x21, 0xEC, 0x40, 0x21, 0xD0, 0xF8 }
for _, write in ipairs({ {2, 0x80}, {7, 0}, {8, 0}, {4, 0}, {0xC, 0}, {0xD, 0}, {1, 0},
                         {3, 0x80}, {5, 0x80}, {6, 0x80}, {0xE, 0x80} }) do
  for _, b in ipairs(mcc(write[1], write[2])) do stub[#stub + 1] = b end
end
for _, b in ipairs({ 0xEA, 0xEA, 0x6C, 0xFC, 0xFF }) do stub[#stub + 1] = b end  -- JMP ($FFFC)

local function on_entry()
  if launched_at and not entered then
    entered = true
    log(string.format("pack entry PC $%06X frame %d", pc24(), frame))
    dump("wram_at_entry.bin", emu.memType.snesWorkRam, 0x20000)
    start_watching()
  end
end
emu.addMemoryCallback(on_entry, cb.exec, 0x00FF2C, 0x00FF2C, emu.cpuType.snes, mem)
emu.addMemoryCallback(on_entry, cb.exec, 0x80FF2C, 0x80FF2C, emu.cpuType.snes, mem)

-- The game's exit stub, which hands back to the BIOS.
local exited = false
emu.addMemoryCallback(function()
  if launched_at and not exited then
    exited = true
    log(string.format("game exit at frame %d (%d frames after launch)", frame, frame - launched_at))
    -- BS-X SRAM by bus address: $10-$17:5000-5FFF, 4 KB per bank.
    local f = assert(io.open(dir .. "/sram_at_exit.bin", "wb"))
    for bank = 0x10, 0x17 do
      for i = 0x5000, 0x5FFF do f:write(string.char(emu.read((bank << 16) | i, mem))) end
    end
    f:close()
    emu.stop(0)
  end
end, cb.exec, 0x105C3C, 0x105C3C, emu.cpuType.snes, mem)

emu.addEventCallback(function()
  frame = frame + 1
  -- Enter through the BIOS's own NMI hook, normally a lone RTL in SRAM. Forcing
  -- the PC with setState can land mid-branch and apply the offset to the new PC.
  if frame == launch_frame then
    for i, b in ipairs(stub) do emu.write(STUB + i - 1, b, mem) end
    emu.write(NMI_HOOK, 0x5C, mem)
    emu.write(NMI_HOOK + 1, STUB & 0xFF, mem)
    emu.write(NMI_HOOK + 2, STUB >> 8, mem)
    emu.write(NMI_HOOK + 3, 0x00, mem)
    -- Answer search_channel_map as if the time channel were in the channel map,
    -- which the BIOS has not downloaded yet: LDA #0 / STA $13FE / CLC / RTL.
    -- Mesen emulates the time channel as channel 0.
    for i, b in ipairs({ 0xA9, 0x00, 0x00, 0x8D, 0xFE, 0x13, 0x18, 0x6B }) do
      emu.write(0x105C08 + i - 1, b, mem)
    end
    launched_at = frame
    log(string.format("launching at frame %d", frame))
  end
  if launched_at and (frame - launched_at) % shot_every == 0 then
    local png = emu.takeScreenshot()
    local f = assert(io.open(string.format("%s/f%07d.png", dir, frame - launched_at), "wb"))
    f:write(png); f:close()
    local m = emu.read(0x7E4183, mem); local s = emu.read(0x7E4184, mem)
    log(string.format("t+%d PC $%06X clock %02d:%02d flags %02X", frame - launched_at, pc24(), m, s, emu.read(0x7E4186, mem)))
  end
  -- Exact capture for frame-by-frame comparison: PVS_SHOT_AT=minute:second:frame.
  if shot_at and entered then
    local at = string.format("%d:%d:%d", emu.read(0x7E4183, mem), emu.read(0x7E4184, mem), emu.read(0x7E4185, mem))
    if at == shot_at then
      local png = emu.takeScreenshot()
      local f = assert(io.open(dir .. "/at.png", "wb"))
      f:write(png); f:close()
      dump("wram_at.bin", emu.memType.snesWorkRam, 0x20000)
      log("captured " .. at .. " at frame " .. frame)
      emu.stop(0)
    end
  end
  if stop_frame > 0 and frame >= stop_frame then
    for k, e in pairs(seen) do log(string.format("count %s x%d", k, e.n)) end
    emu.stop(0)
  end
end, emu.eventType.endFrame)
-- PVS_PRESS_START_AT=<frames after launch>: hold Start for a moment, to leave the
-- final status screen, which waits for it.
if press_start_at > 0 then
  emu.addEventCallback(function()
    if launched_at then
      local t = frame - launched_at
      if t >= press_start_at and t < press_start_at + 10 then
        emu.setInput({ start = true }, 0)
      end
    end
  end, emu.eventType.inputPolled)
end
log("script loaded")
