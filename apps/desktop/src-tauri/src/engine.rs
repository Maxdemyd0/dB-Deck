//! Connection to the native dB-Deck engine.
//!
//! The engine is a separate process that keeps running when the window is
//! closed (or if this UI process crashes). This module finds or starts it,
//! speaks the newline-delimited JSON protocol over the per-user named pipe,
//! forwards engine events to the webview, and reconnects automatically if
//! the engine restarts.

use serde_json::{json, Value};
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicI64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use tauri::{AppHandle, Emitter};
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::net::windows::named_pipe::ClientOptions;
use tokio::sync::{mpsc, oneshot};

pub const PROTOCOL_VERSION: i64 = 1;

type Pending = Arc<Mutex<HashMap<i64, oneshot::Sender<Value>>>>;

pub struct EngineClient {
    tx: Mutex<Option<mpsc::UnboundedSender<String>>>,
    pending: Pending,
    next_id: AtomicI64,
    connected: AtomicBool,
    pub last_state: Mutex<Option<Value>>,
    last_error: Mutex<String>,
}

impl EngineClient {
    pub fn new() -> Self {
        Self {
            tx: Mutex::new(None),
            pending: Arc::new(Mutex::new(HashMap::new())),
            next_id: AtomicI64::new(1),
            connected: AtomicBool::new(false),
            last_state: Mutex::new(None),
            last_error: Mutex::new(String::new()),
        }
    }

    pub fn is_connected(&self) -> bool {
        self.connected.load(Ordering::SeqCst)
    }

    pub fn last_error(&self) -> String {
        self.last_error.lock().unwrap().clone()
    }

    /// Sends a command and waits for the engine's confirmed response.
    pub async fn request(&self, cmd: &str, args: Value) -> Result<Value, Value> {
        let id = self.next_id.fetch_add(1, Ordering::SeqCst);
        let (otx, orx) = oneshot::channel();
        self.pending.lock().unwrap().insert(id, otx);
        let line = json!({ "id": id, "cmd": cmd, "args": args }).to_string();
        let sent = match self.tx.lock().unwrap().as_ref() {
            Some(tx) => tx.send(line).is_ok(),
            None => false,
        };
        if !sent {
            self.pending.lock().unwrap().remove(&id);
            return Err(json!({ "code": "disconnected", "message": "The audio engine is not connected" }));
        }
        match tokio::time::timeout(Duration::from_secs(8), orx).await {
            Ok(Ok(msg)) => {
                if msg.get("ok").and_then(Value::as_bool) == Some(true) {
                    Ok(msg.get("result").cloned().unwrap_or(Value::Null))
                } else {
                    Err(msg.get("error").cloned().unwrap_or(json!({ "code": "internal", "message": "unknown error" })))
                }
            }
            Ok(Err(_)) => Err(json!({ "code": "disconnected", "message": "The audio engine disconnected" })),
            Err(_) => {
                self.pending.lock().unwrap().remove(&id);
                Err(json!({ "code": "timeout", "message": "The audio engine did not respond" }))
            }
        }
    }
}

/// `\\.\pipe\dB-Deck.Engine.v1.<user SID>` — must match the engine.
pub fn pipe_name() -> String {
    format!(r"\\.\pipe\dB-Deck.Engine.v1.{}", current_user_sid().unwrap_or_default())
}

fn current_user_sid() -> Option<String> {
    use windows_sys::Win32::Foundation::{CloseHandle, LocalFree, HANDLE};
    use windows_sys::Win32::Security::Authorization::ConvertSidToStringSidW;
    use windows_sys::Win32::Security::{GetTokenInformation, TokenUser, TOKEN_QUERY, TOKEN_USER};
    use windows_sys::Win32::System::Threading::{GetCurrentProcess, OpenProcessToken};
    unsafe {
        let mut token: HANDLE = std::ptr::null_mut();
        if OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token) == 0 {
            return None;
        }
        let mut size = 0u32;
        GetTokenInformation(token, TokenUser, std::ptr::null_mut(), 0, &mut size);
        let mut buf = vec![0u8; size as usize];
        let ok = GetTokenInformation(token, TokenUser, buf.as_mut_ptr() as *mut _, size, &mut size);
        CloseHandle(token);
        if ok == 0 {
            return None;
        }
        let user = &*(buf.as_ptr() as *const TOKEN_USER);
        let mut wide: *mut u16 = std::ptr::null_mut();
        if ConvertSidToStringSidW(user.User.Sid, &mut wide) == 0 {
            return None;
        }
        let mut len = 0;
        while *wide.add(len) != 0 {
            len += 1;
        }
        let s = String::from_utf16_lossy(std::slice::from_raw_parts(wide, len));
        LocalFree(wide as _);
        Some(s)
    }
}

/// Location of the engine executable: next to the app (installed builds),
/// or the CMake build output during development.
pub fn engine_path() -> Option<PathBuf> {
    if let Ok(p) = std::env::var("DBDECK_ENGINE_PATH") {
        let p = PathBuf::from(p);
        if p.exists() {
            return Some(p);
        }
    }
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            let p = dir.join("dbdeck-engine.exe");
            if p.exists() {
                return Some(p);
            }
        }
    }
    let dev = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../../build/engine-Release/dbdeck-engine.exe");
    if dev.exists() {
        return Some(dev);
    }
    None
}

fn spawn_engine() -> Result<(), String> {
    use std::os::windows::process::CommandExt;
    const CREATE_NO_WINDOW: u32 = 0x0800_0000;
    const CREATE_BREAKAWAY_FROM_JOB: u32 = 0x0100_0000;
    let path = engine_path().ok_or_else(|| "dbdeck-engine.exe was not found".to_string())?;
    let mut cmd = std::process::Command::new(&path);
    cmd.arg("--quiet").stdin(std::process::Stdio::null()).stdout(std::process::Stdio::null()).stderr(std::process::Stdio::null());
    // Break away from any job object so the engine survives a UI crash.
    match cmd.creation_flags(CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB).spawn() {
        Ok(_) => Ok(()),
        Err(_) => cmd
            .creation_flags(CREATE_NO_WINDOW)
            .spawn()
            .map(|_| ())
            .map_err(|e| format!("failed to start {}: {e}", path.display())),
    }
}

fn emit_status(app: &AppHandle, connected: bool, message: &str) {
    let _ = app.emit("engine-status", json!({ "connected": connected, "message": message }));
}

/// Supervises the engine connection forever.
pub fn start(app: AppHandle, client: Arc<EngineClient>, on_state: impl Fn(&AppHandle, &Value) + Send + Sync + 'static) {
    let on_state = Arc::new(on_state);
    tauri::async_runtime::spawn(async move {
        let name = pipe_name();
        let mut last_spawn: Option<Instant> = None;
        loop {
            let pipe = match ClientOptions::new().open(&name) {
                Ok(p) => p,
                Err(e) => {
                    const ERROR_PIPE_BUSY: i32 = 231;
                    if e.raw_os_error() != Some(ERROR_PIPE_BUSY) {
                        // Not running: start it (rate-limited so a crashing engine is not hammered).
                        if last_spawn.map_or(true, |t| t.elapsed() > Duration::from_secs(3)) {
                            last_spawn = Some(Instant::now());
                            match spawn_engine() {
                                Ok(()) => emit_status(&app, false, "Starting audio engine…"),
                                Err(err) => {
                                    *client.last_error.lock().unwrap() = err.clone();
                                    emit_status(&app, false, &err);
                                }
                            }
                        }
                    }
                    tokio::time::sleep(Duration::from_millis(250)).await;
                    continue;
                }
            };

            let (reader, mut writer) = tokio::io::split(pipe);
            let (tx, mut rx) = mpsc::unbounded_channel::<String>();
            *client.tx.lock().unwrap() = Some(tx);

            let writer_task = tokio::spawn(async move {
                while let Some(mut line) = rx.recv().await {
                    line.push('\n');
                    if writer.write_all(line.as_bytes()).await.is_err() {
                        break;
                    }
                }
            });

            // Handshake, then fetch the initial state.
            let reader_client = client.clone();
            let reader_app = app.clone();
            let reader_state = on_state.clone();
            let reader_task = tokio::spawn(async move {
                let mut lines = BufReader::new(reader).lines();
                while let Ok(Some(line)) = lines.next_line().await {
                    let Ok(msg) = serde_json::from_str::<Value>(&line) else { continue };
                    if let Some(id) = msg.get("id").and_then(Value::as_i64) {
                        if let Some(tx) = reader_client.pending.lock().unwrap().remove(&id) {
                            let _ = tx.send(msg);
                        }
                    } else if let Some(ev) = msg.get("event").and_then(Value::as_str) {
                        if ev == "state" {
                            if let Some(data) = msg.get("data") {
                                *reader_client.last_state.lock().unwrap() = Some(data.clone());
                                reader_state(&reader_app, data);
                            }
                        }
                        let _ = reader_app.emit("engine-event", &msg);
                    }
                }
            });

            match client.request("hello", json!({ "protocol": PROTOCOL_VERSION, "client": "dB-Deck UI" })).await {
                Ok(_) => {
                    client.connected.store(true, Ordering::SeqCst);
                    client.last_error.lock().unwrap().clear();
                    emit_status(&app, true, "Connected");
                    if let Ok(state) = client.request("getState", json!({})).await {
                        *client.last_state.lock().unwrap() = Some(state.clone());
                        on_state(&app, &state);
                        let _ = app.emit("engine-event", json!({ "event": "state", "data": state }));
                    }
                }
                Err(e) => {
                    let msg = e.get("message").and_then(Value::as_str).unwrap_or("handshake failed").to_string();
                    *client.last_error.lock().unwrap() = msg.clone();
                    emit_status(&app, false, &msg);
                }
            }

            let _ = reader_task.await;
            writer_task.abort();
            *client.tx.lock().unwrap() = None;
            client.connected.store(false, Ordering::SeqCst);
            // Fail everything still waiting.
            for (_, tx) in client.pending.lock().unwrap().drain() {
                let _ = tx.send(json!({ "ok": false, "error": { "code": "disconnected", "message": "The audio engine disconnected" } }));
            }
            emit_status(&app, false, "Audio engine disconnected — reconnecting…");
            tokio::time::sleep(Duration::from_millis(300)).await;
        }
    });
}
