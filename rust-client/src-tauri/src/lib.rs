//! Tauri backend for the Superlight control panel.
//!
//! The frontend never touches the serial port directly. It calls these
//! commands; the port lives on a dedicated thread in `serial.rs`. Device
//! replies and live events arrive in the frontend as `serial://line` /
//! `serial://status` events.

mod serial;

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

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .manage(SerialState::default())
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
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
