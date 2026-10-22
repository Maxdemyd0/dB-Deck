//! dB-Deck desktop shell: window, system tray, and the bridge to the native
//! audio engine. Closing the window hides it to the tray; the engine keeps
//! running. "Exit dB-Deck" (tray) asks the UI to confirm, then stops the
//! engine and quits.

mod engine;

use engine::EngineClient;
use serde_json::{json, Value};
use std::sync::{Arc, Mutex};
use tauri::menu::{CheckMenuItem, Menu, MenuItem, PredefinedMenuItem, Submenu};
use tauri::tray::{MouseButton, MouseButtonState, TrayIcon, TrayIconBuilder, TrayIconEvent};
use tauri::{AppHandle, Emitter, Manager, State, WindowEvent};
use tauri_plugin_autostart::{MacosLauncher, ManagerExt};

struct AppState {
    engine: Arc<EngineClient>,
    tray: Mutex<Option<TrayIcon>>,
    tray_signature: Mutex<String>,
}

#[tauri::command]
async fn engine_request(state: State<'_, AppState>, cmd: String, args: Option<Value>) -> Result<Value, Value> {
    state.engine.request(&cmd, args.unwrap_or_else(|| json!({}))).await
}

#[tauri::command]
fn engine_status(state: State<'_, AppState>) -> Value {
    json!({
        "connected": state.engine.is_connected(),
        "message": state.engine.last_error(),
        "enginePath": engine::engine_path().map(|p| p.display().to_string()),
    })
}

#[tauri::command]
async fn exit_app(app: AppHandle, state: State<'_, AppState>, stop_engine: bool) -> Result<(), String> {
    if stop_engine {
        let _ = state.engine.request("shutdown", json!({})).await;
    }
    app.exit(0);
    Ok(())
}

#[tauri::command]
fn set_autostart(app: AppHandle, enabled: bool) -> Result<bool, String> {
    let al = app.autolaunch();
    if enabled { al.enable() } else { al.disable() }.map_err(|e| e.to_string())?;
    al.is_enabled().map_err(|e| e.to_string())
}

#[tauri::command]
fn get_autostart(app: AppHandle) -> Result<bool, String> {
    app.autolaunch().is_enabled().map_err(|e| e.to_string())
}

fn show_main(app: &AppHandle) {
    if let Some(w) = app.get_webview_window("main") {
        let _ = w.unminimize();
        let _ = w.show();
        let _ = w.set_focus();
    }
}

/// Builds the tray menu from the engine's confirmed state.
fn build_tray_menu(app: &AppHandle, state: Option<&Value>) -> tauri::Result<Menu<tauri::Wry>> {
    let master_mute = state
        .and_then(|s| s.pointer("/engine/masterMute"))
        .and_then(Value::as_bool)
        .unwrap_or(false);
    let main_status = state.and_then(|s| {
        let profile = s.get("profile")?;
        let main = profile.get("buses")?.as_array()?.iter().find(|b| b.get("isMain") == Some(&Value::Bool(true)))?;
        let id = main.get("id")?.as_str()?;
        let rt = s.pointer(&format!("/runtime/buses/{id}"))?;
        let status = rt.get("status")?.as_str()?.to_string();
        let name = rt.get("activeDeviceName").and_then(Value::as_str).unwrap_or("").to_string();
        Some(match status.as_str() {
            "connected" => format!("Main: {name}"),
            "fallback" => format!("Main (fallback): {name}"),
            "no-destination" => "Main: no output selected".to_string(),
            _ => format!("Main: {name} — disconnected"),
        })
    });

    let open = MenuItem::with_id(app, "open", "Open dB-Deck", true, None::<&str>)?;
    let mute = CheckMenuItem::with_id(app, "mute_all", "Mute All Managed Audio", true, master_mute, None::<&str>)?;
    let profiles = Submenu::with_id(app, "profiles", "Select Profile", true)?;
    if let Some(list) = state.and_then(|s| s.get("profiles")).and_then(Value::as_array) {
        let active = state.and_then(|s| s.get("activeProfileId")).and_then(Value::as_str).unwrap_or("");
        for p in list {
            let id = p.get("id").and_then(Value::as_str).unwrap_or("");
            let name = p.get("name").and_then(Value::as_str).unwrap_or("Profile");
            profiles.append(&CheckMenuItem::with_id(app, format!("profile:{id}"), name, true, id == active, None::<&str>)?)?;
        }
    }
    let status = MenuItem::with_id(
        app,
        "main_status",
        main_status.unwrap_or_else(|| "Audio engine not connected".to_string()),
        false,
        None::<&str>,
    )?;
    let settings = MenuItem::with_id(app, "settings", "Settings", true, None::<&str>)?;
    let exit = MenuItem::with_id(app, "exit", "Exit dB-Deck", true, None::<&str>)?;
    Menu::with_items(
        app,
        &[
            &open,
            &PredefinedMenuItem::separator(app)?,
            &mute,
            &profiles,
            &status,
            &PredefinedMenuItem::separator(app)?,
            &settings,
            &exit,
        ],
    )
}

fn refresh_tray(app: &AppHandle, state: &Value) {
    let st = app.state::<AppState>();
    // Rebuild only when something shown in the menu changed.
    let signature = json!([
        state.pointer("/engine/masterMute"),
        state.get("profiles"),
        state.get("activeProfileId"),
        state.pointer("/runtime/buses"),
    ])
    .to_string();
    {
        let mut sig = st.tray_signature.lock().unwrap();
        if *sig == signature {
            return;
        }
        *sig = signature;
    }
    let tray = st.tray.lock().unwrap().clone();
    if let Some(tray) = tray {
        if let Ok(menu) = build_tray_menu(app, Some(state)) {
            let _ = tray.set_menu(Some(menu));
        }
    }
}

fn handle_menu(app: &AppHandle, id: &str) {
    let engine = app.state::<AppState>().engine.clone();
    match id {
        "open" => show_main(app),
        "settings" => {
            show_main(app);
            let _ = app.emit("navigate", "settings");
        }
        "exit" => {
            // The UI explains what stops working and asks for confirmation.
            show_main(app);
            let _ = app.emit("confirm-exit", ());
        }
        "mute_all" => {
            let muted = engine
                .last_state
                .lock()
                .unwrap()
                .as_ref()
                .and_then(|s| s.pointer("/engine/masterMute").and_then(Value::as_bool))
                .unwrap_or(false);
            tauri::async_runtime::spawn(async move {
                let _ = engine.request("setMasterMute", json!({ "mute": !muted })).await;
            });
        }
        other => {
            if let Some(pid) = other.strip_prefix("profile:") {
                let pid = pid.to_string();
                tauri::async_runtime::spawn(async move {
                    let _ = engine.request("loadProfile", json!({ "id": pid })).await;
                });
            }
        }
    }
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    let engine = Arc::new(EngineClient::new());
    tauri::Builder::default()
        .plugin(tauri_plugin_single_instance::init(|app, _args, _cwd| show_main(app)))
        .plugin(tauri_plugin_autostart::init(MacosLauncher::LaunchAgent, Some(vec!["--minimized"])))
        // Global hotkeys: registered and handled by the UI, which keeps running while hidden in the tray.
        .plugin(tauri_plugin_global_shortcut::Builder::new().build())
        .manage(AppState { engine: engine.clone(), tray: Mutex::new(None), tray_signature: Mutex::new(String::new()) })
        .invoke_handler(tauri::generate_handler![engine_request, engine_status, exit_app, set_autostart, get_autostart])
        .setup(move |app| {
            let handle = app.handle().clone();
            let menu = build_tray_menu(&handle, None)?;
            let tray = TrayIconBuilder::with_id("main")
                .icon(app.default_window_icon().cloned().expect("app icon"))
                .tooltip("dB-Deck")
                .menu(&menu)
                .show_menu_on_left_click(false)
                .on_menu_event(|app, event| handle_menu(app, event.id().as_ref()))
                .on_tray_icon_event(|tray, event| {
                    if let TrayIconEvent::Click { button: MouseButton::Left, button_state: MouseButtonState::Up, .. } = event {
                        show_main(tray.app_handle());
                    }
                })
                .build(app)?;
            *app.state::<AppState>().tray.lock().unwrap() = Some(tray);

            engine::start(handle.clone(), engine.clone(), |app, state| refresh_tray(app, state));

            let minimized = std::env::args().any(|a| a == "--minimized");
            if !minimized {
                show_main(&handle);
            }
            Ok(())
        })
        .on_window_event(|window, event| {
            // Closing the window minimizes to the tray; audio keeps running.
            if let WindowEvent::CloseRequested { api, .. } = event {
                api.prevent_close();
                let _ = window.hide();
            }
        })
        .run(tauri::generate_context!())
        .expect("error while running dB-Deck");
}
