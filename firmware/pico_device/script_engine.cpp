// script_engine.cpp — Lua 5.4 VM on core 1. See script_engine.h for the API.
//
// Threading:
//   core 0 owns the upload buffer and flash; it only ever talks to core 1
//   through the three pico-sdk queues (events in, actions + log out) and a few
//   volatile words it alone writes (run/stop generations).
//   core 1 sits in loop1(). When the run generation moves it builds a fresh
//   lua_State, runs the chunk, then dispatches events to OnEvent() until the
//   stop generation moves. Every VM ends with ACT_RELEASE_ALL, so a stopped or
//   crashed script can never leave a button held down.
//
// Stopping: a count hook checks the stop generation every 1000 VM
// instructions and Sleep() checks it every millisecond, so even
// `while true do end` stops at once. Once stopping, the hook fires on every
// instruction, which gets through a script that wraps its loop in pcall().
//
// Stack: arduino-pico gives core 1 at most 8 KB, which the Lua parser can
// overrun. The VM runs on its own 24 KB stack (run_on_stack), with
// LUAI_MAXCCALLS lowered to 64 in src/lua/luaconf.h, and a canary at the bottom.
//
// Flash: LittleFS (FS partition must be set in the FQBN: flash=2097152_65536).
// Its erase/program paths idle core 1 themselves (rp2040.idleOtherCore()).

#include <Arduino.h>
#include <LittleFS.h>
#include <pico/util/queue.h>
#include <string.h>
#include "script_engine.h"
#include "src/lua/lua.hpp"

// Core 0 has the full 8 KB stack to itself again; core 1 gets its own.
bool core1_separate_stack = true;

static const char *SCRIPT_PATH = "/script.lua";
static const uint32_t LUA_HEAP_MAX = 96 * 1024;
static const uint32_t HOOK_EVERY = 1000;      // VM instructions between stop checks
static const uint32_t DEACTIVATE_MS = 500;    // grace for OnEvent("PROFILE_DEACTIVATED")
static const uint32_t STACK_CANARY = 0x5CA1AB1E;

// ---- shared state ------------------------------------------------------------
static char g_buf[SCRIPT_MAX_LEN];           // uploaded script (core 1 reads it only while loading)
static volatile uint32_t g_len = 0;          // bytes of complete script in g_buf, 0 = none
static uint32_t g_up_expect = 0, g_up_got = 0;
static bool g_uploading = false;
static bool g_saved = false;

static volatile uint32_t g_run_gen = 0;      // core 0: ++ to (re)start
static volatile uint32_t g_stop_gen = 0;     // core 0: ++ to stop
static volatile uint32_t g_run_ack = 0;      // core 1: run gen it has finished loading
static volatile ScriptState g_state = SCRIPT_IDLE;
static volatile bool g_changed = false;
static volatile bool g_ready = false;

static volatile uint8_t g_phys = 0;          // raw physical buttons (IsMouseButtonPressed)
static volatile uint8_t g_block = 0;         // HID bits hidden from the PC
static volatile uint32_t g_mem = 0;
static volatile uint32_t g_log_dropped = 0;

struct BtnEvt { uint8_t pressed, btn; };     // btn: G Hub OnEvent numbering, 1..5
struct LogMsg { uint8_t kind; char text[123]; };
static queue_t q_evt, q_act, q_log;

// G Hub uses two numberings. OnEvent's arg follows the HID order
// (1 left, 2 right, 3 middle, 4 back, 5 forward), but PressMouseButton /
// ReleaseMouseButton / IsMouseButtonPressed swap 2 and 3 (1 left, 2 middle,
// 3 right). Scripts written for G Hub depend on that, so it is kept.
static const uint8_t GHUB_BTN_BIT[6] = { 0, 0x01, 0x04, 0x02, 0x08, 0x10 };

// ---- core 1 helpers ------------------------------------------------------------
static uint32_t s_my_stop = 0;
static bool s_deactivating = false;
static uint32_t s_deadline = 0;
static uint32_t s_start_ms = 0;
static bool s_primary_events = false;

static bool mustAbort() {
  if (s_deactivating) return (int32_t)(millis() - s_deadline) >= 0;
  return g_stop_gen != s_my_stop;
}

static void pushLog(uint8_t kind, const char *s, size_t n) {
  do {
    LogMsg m;
    m.kind = kind;
    size_t k = n < sizeof(m.text) - 1 ? n : sizeof(m.text) - 1;
    memcpy(m.text, s, k);
    m.text[k] = 0;
    if (!queue_try_add(&q_log, &m)) { g_log_dropped++; return; }   // PC not reading: never stall the script
    s += k;  n -= k;
  } while (n);
}
static void logStr(const char *s) { pushLog(LOG_TEXT, s, strlen(s)); }

static void pushAct(lua_State *L, uint8_t kind, int32_t a, int32_t b) {
  ScriptAction act{ kind, a, b };
  while (!queue_try_add(&q_act, &act)) {      // core 0 drains every pass; back-pressure only
    if (mustAbort()) luaL_error(L, "script stopped");
    busy_wait_us(50);
  }
}

static uint8_t checkBtn(lua_State *L, int arg) {
  lua_Integer n = luaL_checkinteger(L, arg);
  if (n < 1 || n > 5) luaL_argerror(L, arg, "mouse button must be 1..5");
  return GHUB_BTN_BIT[n];
}

static int32_t checkRound(lua_State *L, int arg) {
  lua_Number v = luaL_checknumber(L, arg);
  return (int32_t)(v < 0 ? v - 0.5f : v + 0.5f);
}

// ---- script API ------------------------------------------------------------------
static int l_MoveMouseRelative(lua_State *L) {
  pushAct(L, ACT_MOVE, checkRound(L, 1), checkRound(L, 2));
  return 0;
}
static int l_MoveMouseWheel(lua_State *L) {
  pushAct(L, ACT_WHEEL, checkRound(L, 1), 0);
  return 0;
}
static int l_PressMouseButton(lua_State *L) {
  pushAct(L, ACT_PRESS, checkBtn(L, 1), 0);
  return 0;
}
static int l_ReleaseMouseButton(lua_State *L) {
  pushAct(L, ACT_RELEASE, checkBtn(L, 1), 0);
  return 0;
}
static int l_PressAndReleaseMouseButton(lua_State *L) {
  uint8_t bit = checkBtn(L, 1);
  pushAct(L, ACT_PRESS, bit, 0);     // two queue entries = two USB reports, so the click is seen
  pushAct(L, ACT_RELEASE, bit, 0);
  return 0;
}
static int l_IsMouseButtonPressed(lua_State *L) {
  lua_pushboolean(L, (g_phys & checkBtn(L, 1)) != 0);
  return 1;
}
static int l_Sleep(lua_State *L) {
  lua_Integer ms = luaL_optinteger(L, 1, 0);
  uint32_t end = millis() + (ms > 0 ? (uint32_t)ms : 0);
  while ((int32_t)(millis() - end) < 0) {
    if (mustAbort()) return luaL_error(L, "script stopped");
    busy_wait_us(200);
  }
  return 0;
}
static int l_GetRunningTime(lua_State *L) {
  lua_pushinteger(L, (lua_Integer)(millis() - s_start_ms));
  return 1;
}
static int l_OutputLogMessage(lua_State *L) {
  int n = lua_gettop(L);
  lua_getglobal(L, "string");
  lua_getfield(L, -1, "format");
  lua_remove(L, -2);
  lua_insert(L, 1);
  lua_call(L, n, 1);
  size_t len;
  const char *s = lua_tolstring(L, -1, &len);
  if (s) pushLog(LOG_TEXT, s, len);
  return 0;
}
static int l_print(lua_State *L) {
  int n = lua_gettop(L);
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  for (int i = 1; i <= n; i++) {
    if (i > 1) luaL_addchar(&b, '\t');
    luaL_tolstring(L, i, nullptr);
    luaL_addvalue(&b);
  }
  luaL_addchar(&b, '\n');
  luaL_pushresult(&b);
  size_t len;
  const char *s = lua_tolstring(L, -1, &len);
  pushLog(LOG_TEXT, s, len);
  return 0;
}
static int l_ClearLog(lua_State *L) {
  (void)L;
  pushLog(LOG_CLEAR, "", 0);
  return 0;
}
static int l_EnablePrimaryMouseButtonEvents(lua_State *L) {
  s_primary_events = lua_toboolean(L, 1);
  return 0;
}
static int l_SetMouseButtonBlocked(lua_State *L) {
  uint8_t bit = checkBtn(L, 1);
  if (lua_toboolean(L, 2)) g_block |= bit;
  else                     g_block &= ~bit;
  return 0;
}
static int l_false(lua_State *L) {            // keyboard state: this device only sees the mouse
  lua_pushboolean(L, 0);
  return 1;
}
static int l_unsupported(lua_State *L) {
  return luaL_error(L, "%s is not supported on this device (mouse-only, relative movement)",
                    lua_tostring(L, lua_upvalueindex(1)));
}

static void registerApi(lua_State *L) {
  static const luaL_Reg fns[] = {
    { "MoveMouseRelative", l_MoveMouseRelative },
    { "MoveMouseWheel", l_MoveMouseWheel },
    { "PressMouseButton", l_PressMouseButton },
    { "ReleaseMouseButton", l_ReleaseMouseButton },
    { "PressAndReleaseMouseButton", l_PressAndReleaseMouseButton },
    { "IsMouseButtonPressed", l_IsMouseButtonPressed },
    { "Sleep", l_Sleep },
    { "GetRunningTime", l_GetRunningTime },
    { "OutputLogMessage", l_OutputLogMessage },
    { "ClearLog", l_ClearLog },
    { "EnablePrimaryMouseButtonEvents", l_EnablePrimaryMouseButtonEvents },
    { "SetMouseButtonBlocked", l_SetMouseButtonBlocked },
    { "IsModifierPressed", l_false },
    { "IsKeyLockOn", l_false },
    { "print", l_print },
    { nullptr, nullptr },
  };
  lua_pushglobaltable(L);
  luaL_setfuncs(L, fns, 0);
  static const char *const unsupported[] = {
    "PressKey", "ReleaseKey", "PressAndReleaseKey", "MoveMouseTo", "MoveMouseToVirtual",
    "GetMousePosition", "PlayMacro", "AbortMacro", "SetMKeyState", "GetMKeyState", nullptr,
  };
  for (const char *const *u = unsupported; *u; u++) {
    lua_pushstring(L, *u);
    lua_pushcclosure(L, l_unsupported, 1);
    lua_setfield(L, -2, *u);
  }
  lua_pushnil(L);  lua_setfield(L, -2, "dofile");     // no filesystem for scripts
  lua_pushnil(L);  lua_setfield(L, -2, "loadfile");
  lua_pop(L, 1);
}

// ---- VM lifecycle (core 1) -------------------------------------------------------
static void *l_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
  (void)ud;
  if (!ptr) osize = 0;
  if (nsize == 0) {
    free(ptr);
    g_mem -= osize;
    return nullptr;
  }
  if (g_mem - osize + nsize > LUA_HEAP_MAX) return nullptr;   // Lua raises "not enough memory"
  void *p = realloc(ptr, nsize);
  if (p) g_mem = g_mem - osize + nsize;
  return p;
}

static void hook(lua_State *L, lua_Debug *ar) {
  (void)ar;
  if (mustAbort()) {
    lua_sethook(L, hook, LUA_MASKCOUNT, 1);   // re-raise on the very next instruction outside any pcall
    luaL_error(L, "script stopped");
  }
}

static void logError(lua_State *L, const char *where) {
  if (mustAbort()) return;                     // our own "script stopped", not the script's fault
  const char *msg = lua_tostring(L, -1);
  char b[160];
  snprintf(b, sizeof(b), "[%s] %s\n", where, msg ? msg : "(error object is not a string)");
  logStr(b);
}

static void dispatch(lua_State *L, const char *event, int arg, const char *family) {
  lua_getglobal(L, "OnEvent");
  if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }
  lua_pushstring(L, event);
  lua_pushinteger(L, arg);
  lua_pushstring(L, family);
  if (lua_pcall(L, 3, 0, 0) != LUA_OK) {       // G Hub keeps a script alive after an OnEvent error
    logError(L, event);
    lua_pop(L, 1);
  }
}

static int sessionMain(lua_State *L) {
  static const luaL_Reg libs[] = {
    { LUA_GNAME, luaopen_base }, { LUA_COLIBNAME, luaopen_coroutine },
    { LUA_TABLIBNAME, luaopen_table }, { LUA_STRLIBNAME, luaopen_string },
    { LUA_MATHLIBNAME, luaopen_math }, { LUA_UTF8LIBNAME, luaopen_utf8 },
    { nullptr, nullptr },
  };
  for (const luaL_Reg *lib = libs; lib->func; lib++) {
    luaL_requiref(L, lib->name, lib->func, 1);
    lua_pop(L, 1);
  }
  registerApi(L);
  lua_sethook(L, hook, LUA_MASKCOUNT, HOOK_EVERY);

  int rc = luaL_loadbuffer(L, g_buf, g_len, "=script");
  g_run_ack = g_run_gen;                       // g_buf is free for the next upload from here on
  if (rc != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
    logError(L, rc != LUA_OK ? "load" : "run");
    return 0;
  }

  g_state = SCRIPT_RUNNING;
  g_changed = true;
  s_start_ms = millis();

  // Events that arrived while the script was loading are stale.
  BtnEvt e;
  while (queue_try_remove(&q_evt, &e)) {}

  dispatch(L, "PROFILE_ACTIVATED", 0, "");
  while (!mustAbort()) {
    if (queue_try_remove(&q_evt, &e)) {
      if (e.btn == 1 && !s_primary_events) continue;   // G Hub default: left button is not reported
      dispatch(L, e.pressed ? "MOUSE_BUTTON_PRESSED" : "MOUSE_BUTTON_RELEASED", e.btn, "mouse");
    } else {
      busy_wait_us(100);
    }
  }

  s_deactivating = true;
  s_deadline = millis() + DEACTIVATE_MS;
  lua_sethook(L, hook, LUA_MASKCOUNT, HOOK_EVERY);
  dispatch(L, "PROFILE_DEACTIVATED", 0, "");
  return 0;
}

static void session() {
  lua_State *L = lua_newstate(l_alloc, nullptr);
  if (!L) {
    logStr("[load] out of memory\n");
    g_run_ack = g_run_gen;
    return;
  }
  lua_pushcfunction(L, sessionMain);
  if (lua_pcall(L, 0, 0, 0) != LUA_OK) logError(L, "script");
  lua_close(L);
  g_run_ack = g_run_gen;
}

// Call fn() with sp at `top`, then restore. Cortex-M0+ Thumb-1. r4 is
// callee-saved, so fn() hands it back intact.
__attribute__((naked, noinline)) static void run_on_stack(void (*fn)() __attribute__((unused)),
                                                          void *top __attribute__((unused))) {
  asm volatile(
    "push {r4, lr}\n"
    "mov  r4, sp\n"
    "mov  sp, r1\n"
    "blx  r0\n"
    "mov  sp, r4\n"
    "pop  {r4, pc}\n");
}

static uint32_t s_stack[24 * 1024 / 4] __attribute__((aligned(8)));

void setup1() {
  while (!g_ready) tight_loop_contents();     // queues are initialised by scriptBegin() on core 0
}

void loop1() {
  static uint32_t my_run = 0;
  if (g_run_gen == my_run) { busy_wait_us(500); return; }

  my_run = g_run_gen;
  s_my_stop = g_stop_gen;
  s_deactivating = false;
  s_primary_events = false;
  g_mem = 0;
  g_state = SCRIPT_LOADING;
  g_changed = true;

  s_stack[0] = STACK_CANARY;
  run_on_stack(session, &s_stack[sizeof(s_stack) / sizeof(s_stack[0])]);
  if (s_stack[0] != STACK_CANARY) logStr("[engine] script stack overflow\n");

  g_block = 0;
  ScriptAction rel{ ACT_RELEASE_ALL, 0, 0 };
  queue_add_blocking(&q_act, &rel);
  g_state = SCRIPT_IDLE;
  g_changed = true;
}

// ---- core 0 ------------------------------------------------------------------------
void scriptBegin() {
  queue_init(&q_evt, sizeof(BtnEvt), 32);
  queue_init(&q_act, sizeof(ScriptAction), 64);
  queue_init(&q_log, sizeof(LogMsg), 16);
  __dmb();
  g_ready = true;

  if (!LittleFS.begin()) return;               // formats a blank FS partition on first boot
  File f = LittleFS.open(SCRIPT_PATH, "r");
  if (!f) return;
  size_t n = f.size();
  if (n > 0 && n <= SCRIPT_MAX_LEN && f.read((uint8_t *)g_buf, n) == n) {
    g_len = n;
    g_saved = true;
    scriptRun();
  }
  f.close();
}

void scriptOnButtons(uint8_t prev, uint8_t now) {
  g_phys = now;
  if (g_state == SCRIPT_IDLE) return;
  uint8_t diff = (prev ^ now) & 0x1F;
  while (diff) {
    int i = __builtin_ctz(diff);
    diff &= diff - 1;
    BtnEvt e{ (uint8_t)((now >> i) & 1), (uint8_t)(i + 1) };
    queue_try_add(&q_evt, &e);                 // full = script is sleeping through a burst: drop
  }
}

uint8_t scriptBlockedMask() { return g_block; }

bool scriptPollAction(ScriptAction *out) { return queue_try_remove(&q_act, out); }

ScriptLogKind scriptPollLog(char *out, size_t outsz) {
  LogMsg m;
  if (!queue_try_remove(&q_log, &m)) return LOG_NONE;
  strncpy(out, m.text, outsz - 1);
  out[outsz - 1] = 0;
  return m.kind == LOG_CLEAR ? LOG_CLEAR : LOG_TEXT;
}

static bool loaderBusy() { return g_run_gen != g_run_ack; }   // core 1 hasn't finished reading g_buf

const char *scriptUploadBegin(uint32_t len) {
  if (len == 0 || len > SCRIPT_MAX_LEN) return "script_too_big";
  if (loaderBusy()) return "busy";
  g_len = 0;
  g_up_expect = len;
  g_up_got = 0;
  g_uploading = true;
  return nullptr;
}

static int b64val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

const char *scriptUploadChunk(const char *b64, uint32_t *received) {
  if (!g_uploading) return "no_upload";
  uint32_t acc = 0;
  int bits = 0;
  for (const char *p = b64; *p && *p != '='; p++) {
    int v = b64val(*p);
    if (v < 0) { g_uploading = false; return "bad_chunk"; }
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (g_up_got >= g_up_expect) { g_uploading = false; return "too_much_data"; }
      g_buf[g_up_got++] = (char)((acc >> bits) & 0xFF);
    }
  }
  *received = g_up_got;
  return nullptr;
}

const char *scriptUploadEnd() {
  if (!g_uploading) return "no_upload";
  g_uploading = false;
  if (g_up_got != g_up_expect) return "short_upload";
  __dmb();
  g_len = g_up_got;
  return nullptr;
}

const char *scriptRun() {
  if (!g_len) return "no_script";
  if (g_uploading) return "busy";
  __dmb();
  g_stop_gen = g_stop_gen + 1;                 // end the current script (if any)...
  g_run_gen = g_run_gen + 1;                   // ...then core 1 loads the buffer again
  return nullptr;
}

void scriptStop() { g_stop_gen = g_stop_gen + 1; }

const char *scriptSave() {
  if (!g_len || g_uploading) return "no_script";
  File f = LittleFS.open(SCRIPT_PATH, "w");
  if (!f) return "fs_error";
  size_t n = f.write((const uint8_t *)g_buf, g_len);
  f.close();
  if (n != g_len) { LittleFS.remove(SCRIPT_PATH); return "fs_full"; }
  g_saved = true;
  return nullptr;
}

const char *scriptErase() {
  if (LittleFS.exists(SCRIPT_PATH) && !LittleFS.remove(SCRIPT_PATH)) return "fs_error";
  g_saved = false;
  return nullptr;
}

ScriptInfo scriptInfo() {
  return ScriptInfo{ g_state, g_len, g_saved, g_mem, LUA_HEAP_MAX, g_log_dropped };
}

bool scriptStateChanged() {
  if (!g_changed) return false;
  g_changed = false;
  return true;
}
