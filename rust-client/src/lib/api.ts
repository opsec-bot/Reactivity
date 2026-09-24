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
  /** Read-only: which boards are plugged in. Result arrives as `flash://event`s. */
  flashDetect: () => invoke<void>("flash_detect"),
  /** Build + flash. Closes the serial port first. Omit `only` for every detected board. */
  flashStart: (only?: FlashBoard[]) => invoke<void>("flash_start", { only }),
};
