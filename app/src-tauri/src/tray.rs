//! The tray icon and background mode.
//!
//! Closing the window does not stop the nodes. That is the whole point of a
//! system whose value is that data stays in sync across the machines you own:
//! an app you have to leave open is an app that stops syncing the moment
//! anyone tidies their desktop.
//!
//! So the close button hides the window and the app keeps running in the tray,
//! with a menu that says what is happening and offers the two things someone
//! reaches for from a tray: bring it back, or actually quit.
//!
//! Quitting stops every node first. Leaving orphaned `desentryd` processes
//! behind would hold their ports and confuse the next launch -- and a user who
//! chose Quit means it.

use std::sync::atomic::Ordering;
use std::sync::Arc;

use tauri::menu::{Menu, MenuItem, PredefinedMenuItem};
use tauri::tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent};
use tauri::{AppHandle, Manager, Runtime};

use crate::appstate::{self, AppState};

pub fn build<R: Runtime>(app: &AppHandle<R>, state: Arc<AppState>) -> tauri::Result<()> {
    let show = MenuItem::with_id(app, "show", "Open De-Sentry", true, None::<&str>)?;
    let status = MenuItem::with_id(app, "status", "Starting…", false, None::<&str>)?;
    let background = MenuItem::with_id(app, "background", "Keep syncing when closed", true, None::<&str>)?;
    let separator = PredefinedMenuItem::separator(app)?;
    let quit = MenuItem::with_id(app, "quit", "Quit and stop all nodes", true, None::<&str>)?;

    let menu = Menu::with_items(app, &[&status, &separator, &show, &background, &separator, &quit])?;

    let handler_state = Arc::clone(&state);
    TrayIconBuilder::with_id("main")
        .icon(app.default_window_icon().cloned().ok_or_else(|| {
            tauri::Error::AssetNotFound("the tray icon is missing from the bundle".into())
        })?)
        .icon_as_template(true)
        .tooltip("De-Sentry")
        .menu(&menu)
        .show_menu_on_left_click(false)
        .on_menu_event(move |app, event| match event.id().as_ref() {
            "show" => reveal(app),
            "background" => {
                let next = !handler_state.background_mode.load(Ordering::SeqCst);
                handler_state.background_mode.store(next, Ordering::SeqCst);
            }
            "quit" => {
                // Stop the nodes before the process goes away, not after: an
                // exiting parent does not reap its children on any of the three
                // platforms, and orphans would hold their ports.
                appstate::shutdown(&handler_state);
                app.exit(0);
            }
            _ => {}
        })
        .on_tray_icon_event(|tray, event| {
            // Left click brings the window back, which is what people expect
            // from a tray icon on Windows and Linux. macOS opens the menu.
            if let TrayIconEvent::Click {
                button: MouseButton::Left,
                button_state: MouseButtonState::Up,
                ..
            } = event
            {
                reveal(tray.app_handle());
            }
        })
        .build(app)?;

    Ok(())
}

/// Shows and focuses the main window, creating nothing if it is already there.
pub fn reveal<R: Runtime>(app: &AppHandle<R>) {
    if let Some(window) = app.get_webview_window("main") {
        let _ = window.show();
        let _ = window.unminimize();
        let _ = window.set_focus();
    }
}

/// Updates the tray's status line.
///
/// Called after every node state change, so the tray tooltip is accurate
/// without opening the window -- which is the only way to know anything while
/// running in the background.
pub fn update_status<R: Runtime>(app: &AppHandle<R>, running: usize, total: usize, on_battery: bool) {
    let text = if total == 0 {
        "No nodes yet".to_owned()
    } else {
        let mut text = format!("{running} of {total} nodes running");
        if on_battery {
            text.push_str(" · on battery");
        }
        text
    };
    let Some(tray) = app.tray_by_id("main") else { return };
    let _ = tray.set_tooltip(Some(&format!("De-Sentry — {text}")));

    // The tooltip needs a hover; the disabled first menu item is what someone
    // sees the moment they open the menu, so it carries the same line.
    if let Some(menu) = tray.menu() {
        if let Some(item) = menu.get("status") {
            if let Some(item) = item.as_menuitem() {
                let _ = item.set_text(&text);
            }
        }
    }
}
