// Windows: no console window behind the app in a release build. The engine's
// own output is captured by the sidecar and shown in the app, so a console
// would only ever be an empty black rectangle.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    de_sentry_app::run();
}
