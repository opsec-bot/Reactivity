Lua 5.4.8 (https://www.lua.org), MIT license — copyright notice in lua.h.

Vendored for the Pico's on-board script engine (../../script_engine.cpp).
Trimmed: lua.c, luac.c, linit.c, liolib.c, loslib.c, loadlib.c, ldblib.c removed
(no stdio/os/package/debug libraries on the board). Local changes in luaconf.h:
LUA_32BITS=1, LUAI_MAXCCALLS/MAXCCALLS=64 — search for "pico_device".
