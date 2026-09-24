// script_engine.h — on-board Lua scripting (G Hub-style), running on core 1.
//
// Core 0 keeps the passthrough hot path. It feeds button edges in with
// scriptOnButtons() and drains what the script asks for with scriptPollAction()
// / scriptPollLog(). Nothing here ever blocks core 0.
//
// The script API mirrors Logitech G Hub's so existing scripts paste in:
//   OnEvent(event, arg, family)   "PROFILE_ACTIVATED", "PROFILE_DEACTIVATED",
//                                 "MOUSE_BUTTON_PRESSED", "MOUSE_BUTTON_RELEASED"
//   MoveMouseRelative, MoveMouseWheel, PressMouseButton, ReleaseMouseButton,
//   PressAndReleaseMouseButton, IsMouseButtonPressed, Sleep, GetRunningTime,
//   OutputLogMessage, ClearLog, EnablePrimaryMouseButtonEvents
// plus one extension: SetMouseButtonBlocked(n, on) keeps a physical button
// from reaching the PC while the script runs.
#pragma once
#include <stdint.h>
#include <stddef.h>

enum ScriptActKind : uint8_t {
  ACT_MOVE,         // a = dx, b = dy
  ACT_WHEEL,        // a = wheel, b = pan
  ACT_PRESS,        // a = HID button bit
  ACT_RELEASE,      // a = HID button bit
  ACT_RELEASE_ALL,  // script ended: drop everything it holds
};

struct ScriptAction {
  uint8_t kind;
  int32_t a, b;
};

enum ScriptState : uint8_t { SCRIPT_IDLE, SCRIPT_LOADING, SCRIPT_RUNNING };

static const size_t SCRIPT_MAX_LEN = 16 * 1024;

// ---- core 0 ----
void scriptBegin();                                   // mount FS, autorun the saved script
void scriptOnButtons(uint8_t prev, uint8_t now);      // hot path: raw physical button edges
uint8_t scriptBlockedMask();                          // HID bits the script is hiding from the PC
bool scriptPollAction(ScriptAction *out);
// One piece of script output: LOG_TEXT (NUL-terminated text, newlines are the
// script's own, like G Hub's console), LOG_CLEAR (ClearLog()), or LOG_NONE.
enum ScriptLogKind { LOG_NONE, LOG_TEXT, LOG_CLEAR };
ScriptLogKind scriptPollLog(char *out, size_t outsz);

// Upload: begin(len) -> chunk(base64)... -> end(). Returns nullptr or an error code.
const char *scriptUploadBegin(uint32_t len);
const char *scriptUploadChunk(const char *b64, uint32_t *received);
const char *scriptUploadEnd();
const char *scriptRun();                              // (re)start the uploaded script
void scriptStop();
const char *scriptSave();                             // persist -> autoruns at boot
const char *scriptErase();                            // forget the saved script

// Snapshot for the status line / UI.
struct ScriptInfo {
  ScriptState state;
  uint32_t len;          // bytes of script in RAM (0 = none)
  bool saved;            // a script is stored in flash
  uint32_t mem;          // Lua heap in use
  uint32_t mem_max;      // Lua heap cap
  uint32_t log_dropped;  // log lines lost because the PC wasn't reading
};
ScriptInfo scriptInfo();
bool scriptStateChanged();                            // true once per state change (core 0 poll)
