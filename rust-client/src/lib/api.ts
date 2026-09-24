import { invoke } from "@tauri-apps/api/core";

export interface PortInfo {
  name: string;
  label: string;
  is_pico: boolean;
}

export interface StatusPayload {
  connected: boolean;
  port: string | null;
  error: string | null;
}

/** A `status` reply from the Pico. Fields are best-effort (firmware-defined). */
export interface DeviceStatus {
  type: "status";
  uptime_ms?: number;
  esp_alive?: boolean;
  frames_ok?: number;
  frames_bad?: number;
  watching?: boolean;
  [k: string]: unknown;
}

/** A live mouse event echoed while watching. */
export interface MouseEvt {
  type: "evt";
  kind: "mouse";
  buttons: number;
  dx: number;
  dy: number;
  wheel?: number;
  [k: string]: unknown;
}

export interface AckMsg {
  type: "ack";
  what?: string;
  [k: string]: unknown;
}

/** State of the on-board Lua script (`script_status` line). */
export interface ScriptStatus {
  type: "script_status";
  state: "idle" | "loading" | "running";
  /** Bytes of script in the board's RAM (0 = none uploaded). */
  len: number;
  /** A script is stored in flash and starts at boot. */
  saved: boolean;
  /** Lua heap in use / cap, bytes. */
  mem: number;
  mem_max: number;
  log_dropped: number;
}

/** Script output: `text` is raw (newlines are the script's), `clear` = ClearLog(). */
export interface ScriptLog {
  type: "script_log";
  text?: string;
  clear?: boolean;
}

export type ScriptAction = "run" | "stop" | "save" | "erase" | "status";

/** Largest script the board accepts (SCRIPT_MAX_LEN in script_engine.h). */
export const SCRIPT_MAX_LEN = 16 * 1024;

export type DeviceMessage = DeviceStatus | MouseEvt | AckMsg | Record<string, unknown>;

export type MouseButton = "left" | "right" | "middle" | "side1" | "side2";

// ---- Firmware build + flash (scripts/flash.py --json, forwarded as `flash://event`) ----

export type FlashBoard = "esp32" | "pico";
export type FlashStepName = "build" | "flash" | "verify";

export interface FlashFound {
  board: FlashBoard;
  port: string;
  /** "serial" for a COM port, "uf2conv" for a Pico sitting in BOOTSEL. */
  protocol: string;
  state: string;
  ident: string;
}

/** One line of the script's output. `error`, `stderr` and `exit` are added by the backend. */
export type FlashEvent =
  | { event: "detect"; found: FlashFound[]; skipped: [FlashBoard, string][]; notes: string[] }
  | {
      event: "step";
      board: FlashBoard;
      step: FlashStepName;
      status: "start" | "ok" | "fail";
      detail: string;
      seconds: number | null;
    }
  | { event: "log"; board: FlashBoard; line: string }
  | { event: "summary"; results: Partial<Record<FlashBoard, "ok" | "fail">>; ok: boolean }
  | { event: "error"; message: string }
  | { event: "stderr"; text: string }
  | { event: "exit"; code: number };

export const api = {
  listPorts: () => invoke<PortInfo[]>("list_serial_ports"),
  connect: (port: string, baud?: number) =>
    invoke<void>("connect_serial", { port, baud }),
  disconnect: () => invoke<void>("disconnect_serial"),
  connectionState: () => invoke<boolean>("connection_state"),
  sendCommand: (json: string) => invoke<void>("send_command", { json }),
  move: (dx: number, dy: number) => invoke<void>("mouse_move", { dx, dy }),
  click: (btn: MouseButton) => invoke<void>("mouse_click", { btn }),
  scroll: (wheel: number) => invoke<void>("mouse_scroll", { wheel }),
  setRemap: (from: string, to: string) => invoke<void>("set_remap", { from, to }),
  requestStatus: () => invoke<void>("request_status"),
  setWatch: (on: boolean) => invoke<void>("set_watch", { on }),
  /** Send a Lua script to the board; `run` starts it (replacing the running one). */
  scriptUpload: (source: string, run: boolean) =>
    invoke<void>("script_upload", { source, run }),
  scriptCommand: (action: ScriptAction) => invoke<void>("script_command", { action }),
  /** Read-only: which boards are plugged in. Result arrives as `flash://event`s. */
  flashDetect: () => invoke<void>("flash_detect"),
  /** Build + flash. Closes the serial port first. Omit `only` for every detected board. */
  flashStart: (only?: FlashBoard[]) => invoke<void>("flash_start", { only }),
};
