//! Serial link to the Pico (USB CDC, JSON lines).
//!
//! One dedicated OS thread owns the port and does blocking reads with a short
//! timeout; outgoing command lines arrive over an mpsc channel. No async, no
//! tokio in this layer (HANDOFF §11.6) — Tauri's own runtime is unrelated.
//!
//! Wire protocol (HANDOFF §9, Pico <-> PC): one JSON object per line, `\n`
//! terminated. Incoming lines are forwarded verbatim to the frontend as
//! `serial://line` events; the frontend parses them.

use std::io::{Read, Write};
use std::sync::mpsc::{self, Sender, TryRecvError};
use std::sync::Mutex;
use std::time::Duration;

use serde::Serialize;
use tauri::{AppHandle, Emitter};

/// A serial port we could connect to, with a friendly label.
#[derive(Debug, Clone, Serialize)]
pub struct PortInfo {
    pub name: String,
    pub label: String,
    /// True if this looks like our Pico (RP2040 TinyUSB, 239A:CAFE).
    pub is_pico: bool,
}

/// Status pushed to the frontend whenever the connection state changes.
#[derive(Debug, Clone, Serialize)]
pub struct StatusPayload {
    pub connected: bool,
    pub port: Option<String>,
    pub error: Option<String>,
}

/// Message to the port-owning thread.
enum ToDevice {
    Send(String),
    Disconnect,
}

/// Connection handle stored in Tauri state.
#[derive(Default)]
pub struct SerialState {
    inner: Mutex<Option<Sender<ToDevice>>>,
}

impl SerialState {
    fn set(&self, tx: Option<Sender<ToDevice>>) {
        *self.inner.lock().unwrap() = tx;
    }

    fn sender(&self) -> Option<Sender<ToDevice>> {
        self.inner.lock().unwrap().clone()
    }

    pub fn is_connected(&self) -> bool {
        self.inner.lock().unwrap().is_some()
    }
}

const PICO_VID: u16 = 0x239A;
const PICO_PID: u16 = 0xCAFE;

/// Enumerate serial ports, labelling our Pico if we can spot it.
pub fn list_ports() -> Result<Vec<PortInfo>, String> {
    let ports = serialport::available_ports().map_err(|e| e.to_string())?;
    let mut out = Vec::new();
    for p in ports {
        let (label, is_pico) = match &p.port_type {
            serialport::SerialPortType::UsbPort(info) => {
                let product = info.product.clone().unwrap_or_default();
                let is_pico = info.vid == PICO_VID && info.pid == PICO_PID;
                let tag = if product.is_empty() {
                    format!("{:04X}:{:04X}", info.vid, info.pid)
                } else {
                    format!("{} ({:04X}:{:04X})", product, info.vid, info.pid)
                };
                (format!("{} — {}", p.port_name, tag), is_pico)
            }
            _ => (p.port_name.clone(), false),
        };
        out.push(PortInfo {
            name: p.port_name.clone(),
            label,
            is_pico,
        });
    }
    // Pico first, then by name.
    out.sort_by(|a, b| b.is_pico.cmp(&a.is_pico).then(a.name.cmp(&b.name)));
    Ok(out)
}

/// Open `port_name` at `baud` and spawn the owning thread. Replaces any
/// existing connection.
pub fn connect(
    app: AppHandle,
    state: &SerialState,
    port_name: String,
    baud: u32,
) -> Result<(), String> {
    // Tear down any prior connection first.
    disconnect(state);

    let mut port = serialport::new(&port_name, baud)
        .timeout(Duration::from_millis(50))
        .open()
        .map_err(|e| format!("open {port_name}: {e}"))?;

    // CRITICAL: Adafruit TinyUSB CDC gates device->host writes on DTR
    // (tud_cdc_connected()). serialport-rs leaves DTR low on open, which makes
    // the Pico silently drop every status/evt/ack line. Assert DTR (and RTS) so
    // it talks back. (pyserial asserts DTR by default — that's why it differed.)
    let _ = port.write_data_terminal_ready(true);
    let _ = port.write_request_to_send(true);

    let (tx, rx) = mpsc::channel::<ToDevice>();
    state.set(Some(tx));

    let app_thread = app.clone();
    let name_thread = port_name.clone();
    std::thread::Builder::new()
        .name(format!("serial-{port_name}"))
        .spawn(move || reader_loop(app_thread, name_thread, port, rx))
        .map_err(|e| e.to_string())?;

    let _ = app.emit(
        "serial://status",
        StatusPayload {
            connected: true,
            port: Some(port_name),
            error: None,
        },
    );
    Ok(())
}

/// Signal the owning thread to stop and clear the handle.
pub fn disconnect(state: &SerialState) {
    if let Some(tx) = state.sender() {
        let _ = tx.send(ToDevice::Disconnect);
    }
    state.set(None);
}

/// Queue one JSON command line for the device.
pub fn send_line(state: &SerialState, line: String) -> Result<(), String> {
    let tx = state.sender().ok_or("not connected")?;
    tx.send(ToDevice::Send(line))
        .map_err(|_| "serial thread gone".to_string())
}

/// The port-owning thread: drain outgoing queue, then read available bytes and
/// emit complete lines. Exits on Disconnect, a dropped channel, or a fatal read
/// error (which it reports as a status event).
fn reader_loop(
    app: AppHandle,
    port_name: String,
    mut port: Box<dyn serialport::SerialPort>,
    rx: mpsc::Receiver<ToDevice>,
) {
    let mut buf = [0u8; 1024];
    let mut line: Vec<u8> = Vec::with_capacity(256);

    loop {
        // 1) Outgoing commands (non-blocking).
        loop {
            match rx.try_recv() {
                Ok(ToDevice::Send(s)) => {
                    let _ = port.write_all(s.as_bytes());
                    let _ = port.write_all(b"\n");
                    let _ = port.flush();
                }
                Ok(ToDevice::Disconnect) | Err(TryRecvError::Disconnected) => {
                    let _ = app.emit(
                        "serial://status",
                        StatusPayload {
                            connected: false,
                            port: Some(port_name.clone()),
                            error: None,
                        },
                    );
                    return;
                }
                Err(TryRecvError::Empty) => break,
            }
        }

        // 2) Incoming bytes (blocks up to the 50ms timeout).
        match port.read(&mut buf) {
            Ok(0) => {}
            Ok(n) => {
                for &b in &buf[..n] {
                    match b {
                        b'\n' => {
                            let s = String::from_utf8_lossy(&line).trim().to_string();
                            if !s.is_empty() {
                                let _ = app.emit("serial://line", s);
                            }
                            line.clear();
                        }
                        b'\r' => {}
                        _ => {
                            if line.len() < 8192 {
                                line.push(b);
                            } else {
                                line.clear(); // runaway line guard
                            }
                        }
                    }
                }
            }
            Err(ref e) if e.kind() == std::io::ErrorKind::TimedOut => {}
            Err(e) => {
                let _ = app.emit(
                    "serial://status",
                    StatusPayload {
                        connected: false,
                        port: Some(port_name.clone()),
                        error: Some(e.to_string()),
                    },
                );
                return;
            }
        }
    }
}
