//! Build + flash the two boards by driving `scripts/flash.py --json`.
//!
//! The script does the real work (detect boards, arduino-cli build, upload,
//! verify); this module only runs it on a worker thread and forwards every JSON
//! event line to the frontend as a `flash://event`. Same style as `serial.rs`:
//! plain std threads, no async.
//!
//! Besides the script's own events (`detect`, `step`, `log`, `summary`) two
//! synthetic ones are added so the UI always knows how a run ended:
//!   {"event":"error","message":...}  the script could not be started
//!   {"event":"stderr","text":...}    anything the script wrote to stderr
//!   {"event":"exit","code":N}        always the last event of a run

use std::io::{BufRead, BufReader, Read};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use serde_json::json;
use tauri::{AppHandle, Emitter};

/// Boards `flash.py --only` understands. User-supplied names are checked against
/// this before anything reaches a subprocess.
const BOARDS: [&str; 2] = ["esp32", "pico"];

/// One detect/flash run at a time (two uploads at once would fight over ports).
#[derive(Default)]
pub struct FlashState {
    running: Arc<AtomicBool>,
}

/// Holds the "running" flag; releasing it on drop means the flag clears even if
/// the worker thread panics.
pub struct RunGuard(Arc<AtomicBool>);

impl Drop for RunGuard {
    fn drop(&mut self) {
        self.0.store(false, Ordering::SeqCst);
    }
}

impl FlashState {
    pub fn try_begin(&self) -> Option<RunGuard> {
        if self.running.swap(true, Ordering::SeqCst) {
            None
        } else {
            Some(RunGuard(self.running.clone()))
        }
    }
}

/// Arguments for `flash.py`. `check` = detect only. Unknown board names are rejected.
pub fn build_args(check: bool, only: &[String]) -> Result<Vec<String>, String> {
    let mut args = vec!["--json".to_string()];
    if check {
        args.push("--check".into());
    }
    for board in only {
        if !BOARDS.contains(&board.as_str()) {
            return Err(format!("unknown board '{board}'"));
        }
        args.push("--only".into());
        args.push(board.clone());
    }
    Ok(args)
}

/// `<repo>/scripts/flash.py`, located from this crate's compile-time path
/// (src-tauri -> rust-client -> repo root). Override with SUPERLIGHT_FLASH_SCRIPT.
fn script_path() -> Result<PathBuf, String> {
    let path = match std::env::var_os("SUPERLIGHT_FLASH_SCRIPT") {
        Some(p) => PathBuf::from(p),
        None => Path::new(env!("CARGO_MANIFEST_DIR"))
            .parent()
            .and_then(Path::parent)
            .ok_or("cannot locate the repo root")?
            .join("scripts")
            .join("flash.py"),
    };
    if path.is_file() {
        Ok(path)
    } else {
        Err(format!(
            "flash script not found: {} (set SUPERLIGHT_FLASH_SCRIPT to override)",
            path.display()
        ))
    }
}

/// Start the script under whichever Python is available.
fn spawn_python(script: &Path, args: &[String]) -> Result<Child, String> {
    let mut candidates: Vec<(String, Vec<&str>)> = Vec::new();
    if let Ok(p) = std::env::var("SUPERLIGHT_PYTHON") {
        candidates.push((p, vec![]));
    }
    candidates.push(("python".into(), vec![]));
    candidates.push(("py".into(), vec!["-3"]));

    let root = script.parent().and_then(Path::parent).unwrap_or(Path::new("."));
    let mut last_err = String::new();
    for (exe, pre) in candidates {
        let mut cmd = Command::new(&exe);
        cmd.args(&pre)
            .arg("-u") // unbuffered: events must reach the UI as they happen
            .arg(script)
            .args(args)
            .current_dir(root)
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            cmd.creation_flags(0x0800_0000); // CREATE_NO_WINDOW: no console flash from a GUI app
        }
        match cmd.spawn() {
            Ok(child) => return Ok(child),
            Err(e) => last_err = format!("{exe}: {e}"),
        }
    }
    Err(format!(
        "could not start Python ({last_err}). Install Python 3 or set SUPERLIGHT_PYTHON."
    ))
}

/// Keep only the tail of a (possibly huge) stderr dump.
fn tail(s: &str, max: usize) -> String {
    let s = s.trim();
    if s.len() <= max {
        return s.to_string();
    }
    let mut cut = s.len() - max;
    while !s.is_char_boundary(cut) {
        cut += 1;
    }
    format!("...{}", &s[cut..])
}

/// Run the script, handing every stdout line (a JSON event, verbatim) to
/// `on_line`. Returns the exit code. Blocking.
pub fn run_script(args: &[String], mut on_line: impl FnMut(String)) -> Result<i32, String> {
    let script = script_path()?;
    let mut child = spawn_python(&script, args)?;

    // Drain stderr on its own thread so a chatty child can never block on a full pipe.
    let stderr = child.stderr.take();
    let stderr_thread = std::thread::spawn(move || {
        let mut text = String::new();
        if let Some(mut e) = stderr {
            let _ = e.read_to_string(&mut text);
        }
        text
    });

    if let Some(out) = child.stdout.take() {
        let mut reader = BufReader::new(out);
        let mut buf = Vec::new();
        while reader.read_until(b'\n', &mut buf).map(|n| n > 0).unwrap_or(false) {
            let line = String::from_utf8_lossy(&buf).trim().to_string();
            buf.clear();
            if !line.is_empty() {
                on_line(line);
            }
        }
    }

    let status = child.wait().map_err(|e| e.to_string())?;
    let stderr_text = stderr_thread.join().unwrap_or_default();
    if !stderr_text.trim().is_empty() {
        on_line(json!({ "event": "stderr", "text": tail(&stderr_text, 2000) }).to_string());
    }
    Ok(status.code().unwrap_or(-1))
}

/// Begin a run on a worker thread. `prepare` runs first, synchronously, but only
/// once we know this run is allowed to start (used to release the serial port).
/// Progress arrives as `flash://event` lines; the last one is always `exit`.
pub fn start(
    app: AppHandle,
    state: &FlashState,
    args: Vec<String>,
    prepare: impl FnOnce(),
) -> Result<(), String> {
    let guard = state
        .try_begin()
        .ok_or("a flash or detection is already running")?;
    prepare();
    std::thread::Builder::new()
        .name("flash".into())
        .spawn(move || {
            // `guard` is owned by this closure, so the flag clears even if we panic.
            let emit = |line: String| {
                let _ = app.emit("flash://event", line);
            };
            let code = match run_script(&args, |l| emit(l)) {
                Ok(code) => code,
                Err(message) => {
                    emit(json!({ "event": "error", "message": message }).to_string());
                    -1
                }
            };
            // Release BEFORE announcing the end: the UI reacts to `exit` by starting
            // a fresh detection, which must not be refused as "already running".
            drop(guard);
            emit(json!({ "event": "exit", "code": code }).to_string());
        })
        .map(|_| ())
        .map_err(|e| e.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn args_always_ask_for_json() {
        assert_eq!(build_args(false, &[]).unwrap(), vec!["--json"]);
        assert_eq!(build_args(true, &[]).unwrap(), vec!["--json", "--check"]);
    }

    #[test]
    fn only_is_forwarded_per_board() {
        let args = build_args(false, &["pico".into(), "esp32".into()]).unwrap();
        assert_eq!(args, vec!["--json", "--only", "pico", "--only", "esp32"]);
    }

    #[test]
    fn unknown_or_hostile_board_names_are_rejected() {
        assert!(build_args(false, &["toaster".into()]).is_err());
        assert!(build_args(false, &["pico; calc".into()]).is_err());
        assert!(build_args(false, &["--port".into()]).is_err());
        assert!(build_args(false, &["".into()]).is_err());
    }

    #[test]
    fn only_one_run_at_a_time() {
        let state = FlashState::default();
        let first = state.try_begin();
        assert!(first.is_some());
        assert!(state.try_begin().is_none(), "second run must be refused");
        drop(first);
        assert!(state.try_begin().is_some(), "flag must clear when the guard drops");
    }

    #[test]
    fn guard_clears_even_if_the_worker_panics() {
        let state = FlashState::default();
        let guard = state.try_begin().unwrap();
        let _ = std::thread::spawn(move || {
            let _guard = guard;
            panic!("worker died");
        })
        .join();
        assert!(state.try_begin().is_some());
    }

    #[test]
    fn script_is_found_in_this_repo() {
        let p = script_path().expect("scripts/flash.py should exist");
        assert!(p.ends_with("scripts/flash.py"), "{}", p.display());
    }

    #[test]
    fn tail_keeps_the_end_and_respects_char_boundaries() {
        assert_eq!(tail("  short  ", 100), "short");
        let long = "é".repeat(50); // 2 bytes per char
        let t = tail(&long, 11);
        assert!(t.starts_with("..."));
        assert!(t.len() <= 3 + 11);
    }

    /// Real end-to-end: spawns Python + arduino-cli. Read-only (`--check`).
    /// Run with: cargo test -- --ignored
    #[test]
    #[ignore = "needs python and arduino-cli; run with --ignored"]
    fn check_json_round_trip() {
        let mut lines = Vec::new();
        let code = run_script(&build_args(true, &[]).unwrap(), |l| lines.push(l)).unwrap();
        // 0 = at least one board found, 2 = none plugged in; anything else is a failure.
        assert!(code == 0 || code == 2, "exit code {code}, lines: {lines:?}");
        let detect = lines
            .iter()
            .filter_map(|l| serde_json::from_str::<serde_json::Value>(l).ok())
            .find(|v| v["event"] == "detect")
            .unwrap_or_else(|| panic!("no detect event in {lines:?}"));
        assert!(detect["found"].is_array() && detect["skipped"].is_array());
    }
}
