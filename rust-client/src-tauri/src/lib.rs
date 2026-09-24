//! Tauri backend for the Superlight control panel.
//!
//! The frontend never touches the serial port directly. It calls these
//! commands; the port lives on a dedicated thread in `serial.rs`. Device
//! replies and live events arrive in the frontend as `serial://line` /
//! `serial://status` events.

mod flash;
mod serial;

use flash::FlashState;
use serde_json::json;
use serial::{PortInfo, SerialState};
use tauri::{AppHandle, State};

/// Pico CDC runs at 115200 (matches firmware `Serial.begin(115200)`).
const DEFAULT_BAUD: u32 = 115200;

#[tauri::command]
fn list_serial_ports() -> Result<Vec<PortInfo>, String> {
    serial::list_ports()
}

#[tauri::command]
fn connect_serial(
    app: AppHandle,
    state: State<'_, SerialState>,
    port: String,
    baud: Option<u32>,
) -> Result<(), String> {
    serial::connect(app, &state, port, baud.unwrap_or(DEFAULT_BAUD))
}

#[tauri::command]
fn disconnect_serial(state: State<'_, SerialState>) -> Result<(), String> {
    serial::disconnect(&state);
    Ok(())
}

#[tauri::command]
fn connection_state(state: State<'_, SerialState>) -> bool {
    state.is_connected()
}

/// Send a raw JSON object (already-serialized) as one line. Escape hatch /
/// power users; the typed commands below are preferred.
#[tauri::command]
fn send_command(state: State<'_, SerialState>, json: String) -> Result<(), String> {
    serial::send_line(&state, json)
}

#[tauri::command]
fn mouse_move(state: State<'_, SerialState>, dx: i32, dy: i32) -> Result<(), String> {
    serial::send_line(&state, json!({ "cmd": "move", "dx": dx, "dy": dy }).to_string())
}

#[tauri::command]
fn mouse_click(state: State<'_, SerialState>, btn: String) -> Result<(), String> {
    serial::send_line(&state, json!({ "cmd": "click", "btn": btn }).to_string())
}

#[tauri::command]
fn mouse_scroll(state: State<'_, SerialState>, wheel: i32) -> Result<(), String> {
    serial::send_line(&state, json!({ "cmd": "scroll", "wheel": wheel }).to_string())
}

#[tauri::command]
fn set_remap(state: State<'_, SerialState>, from: String, to: String) -> Result<(), String> {
    serial::send_line(
        &state,
        json!({ "cmd": "remap", "from": from, "to": to }).to_string(),
    )
}

#[tauri::command]
fn request_status(state: State<'_, SerialState>) -> Result<(), String> {
    serial::send_line(&state, json!({ "cmd": "status" }).to_string())
}

/// Enable/disable the live event stream from the Pico (passthrough events get
/// echoed over CDC as `evt` lines while watching is on).
#[tauri::command]
fn set_watch(state: State<'_, SerialState>, on: bool) -> Result<(), String> {
    serial::send_line(&state, json!({ "cmd": "watch", "on": on }).to_string())
}

/// Largest script the Pico accepts (`SCRIPT_MAX_LEN` in script_engine.h).
const SCRIPT_MAX_LEN: usize = 16 * 1024;
/// Script bytes per `script_chunk` line: 150 bytes = 200 base64 chars, which
/// keeps each line inside the firmware's 256-byte command buffer.
const SCRIPT_CHUNK: usize = 150;

/// Upload a Lua script to the Pico (begin -> chunks -> end) and, if `run`,
/// start it straight away. Progress and errors come back as `ack` / `error`
/// lines; the script's own output as `script_log` lines.
#[tauri::command]
fn script_upload(state: State<'_, SerialState>, source: String, run: bool) -> Result<(), String> {
    use base64::Engine as _;
    let bytes = source.as_bytes();
    if bytes.is_empty() {
        return Err("script is empty".into());
    }
    if bytes.len() > SCRIPT_MAX_LEN {
        return Err(format!(
            "script is {} bytes; the board holds at most {SCRIPT_MAX_LEN}",
            bytes.len()
        ));
    }
    serial::send_line(&state, json!({ "cmd": "script_begin", "len": bytes.len() }).to_string())?;
    for chunk in bytes.chunks(SCRIPT_CHUNK) {
        let d = base64::engine::general_purpose::STANDARD.encode(chunk);
        serial::send_line(&state, json!({ "cmd": "script_chunk", "d": d }).to_string())?;
    }
    serial::send_line(&state, json!({ "cmd": "script_end", "run": run }).to_string())
}

/// run | stop | save | erase | status for the script already on the board.
#[tauri::command]
fn script_command(state: State<'_, SerialState>, action: String) -> Result<(), String> {
    if !matches!(action.as_str(), "run" | "stop" | "save" | "erase" | "status") {
        return Err(format!("unknown script action: {action}"));
    }
    serial::send_line(&state, json!({ "cmd": format!("script_{action}") }).to_string())
}

/// Detect which boards are plugged in (read-only). Progress arrives as
/// `flash://event` lines and always ends with an `exit` event.
#[tauri::command]
fn flash_detect(app: AppHandle, flash: State<'_, FlashState>) -> Result<(), String> {
    flash::start(app, &flash, flash::build_args(true, &[])?, || {})
}

/// Build and flash the boards that are plugged in (or only `only`, a subset of
/// "esp32" / "pico"). The serial connection is closed first — an open COM port
/// cannot be flashed — and the frontend reconnects when the run ends.
#[tauri::command]
fn flash_start(
    app: AppHandle,
    serial: State<'_, SerialState>,
    flash: State<'_, FlashState>,
    only: Option<Vec<String>>,
) -> Result<(), String> {
    let args = flash::build_args(false, &only.unwrap_or_default())?;
    flash::start(app, &flash, args, || serial::disconnect_blocking(&serial))
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .manage(SerialState::default())
        .manage(FlashState::default())
        .invoke_handler(tauri::generate_handler![
            list_serial_ports,
            connect_serial,
            disconnect_serial,
            connection_state,
            send_command,
            mouse_move,
            mouse_click,
            mouse_scroll,
            set_remap,
            request_status,
            set_watch,
            script_upload,
            script_command,
            flash_detect,
            flash_start,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
