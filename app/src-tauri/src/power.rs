//! Battery and connectivity watching, and what the app does about them.
//!
//! Two behaviours the spec asks for, and one honest limitation.
//!
//! **On battery, gossip is throttled.** A laptop in a bag running fifty gossip
//! rounds a minute is a laptop with no battery left. The throttle is applied by
//! the engine (`GossipService::SetThrottleFactor`); this module's job is only
//! to notice the power source changing and tell the nodes.
//!
//! **On a network change, discovery is nudged.** Peers found on a previous
//! WiFi network are unreachable on the new one, and waiting for them to time
//! out makes the app look broken for a minute after moving desks. A network
//! change wakes discovery so the peer table rebuilds immediately.
//!
//! **The limitation:** polling. There is a proper event for each of these on
//! each platform -- `WM_POWERBROADCAST` and `WM_DEVICECHANGE` on Windows,
//! `IOKit` and `SCNetworkReachability` on macOS, D-Bus signals on Linux -- and
//! each requires a message loop or a service connection that this process does
//! not otherwise need. Polling every few seconds costs nothing measurable and
//! is the same on all three platforms, which is worth more here than a
//! three-implementation event path. The interval is stated rather than hidden
//! so the trade is visible.

use std::sync::atomic::Ordering;
use std::sync::Arc;
use std::time::Duration;

use tauri::AppHandle;

use crate::appstate::{emit, AppState, SidecarEvent};

/// How often the power source and volume set are checked.
const POLL: Duration = Duration::from_secs(5);

/// Gossip interval multiplier while on battery. Four times slower still
/// converges in seconds on a quiet LAN, and cuts the radio duty cycle enough
/// to matter.
pub const BATTERY_THROTTLE: u32 = 4;

/// True when the machine is running on battery.
#[cfg(windows)]
pub fn on_battery() -> bool {
    use windows_sys::Win32::System::Power::{GetSystemPowerStatus, SYSTEM_POWER_STATUS};

    // SAFETY: the struct is plain data and the call only writes into it.
    let mut status: SYSTEM_POWER_STATUS = unsafe { std::mem::zeroed() };
    let ok = unsafe { GetSystemPowerStatus(&mut status) };
    // ACLineStatus: 0 offline, 1 online, 255 unknown. Unknown is treated as
    // mains -- throttling a desktop that cannot report its power source would
    // be a silent slowdown with no cause the user could find.
    ok != 0 && status.ACLineStatus == 0
}

#[cfg(target_os = "macos")]
pub fn on_battery() -> bool {
    // `pmset -g batt` reports the source in its first line. Shelling out for a
    // value read every five seconds is cheap, and it avoids linking IOKit for
    // one boolean.
    std::process::Command::new("/usr/bin/pmset")
        .args(["-g", "batt"])
        .output()
        .ok()
        .map(|out| String::from_utf8_lossy(&out.stdout).contains("Battery Power"))
        .unwrap_or(false)
}

#[cfg(all(unix, not(target_os = "macos")))]
pub fn on_battery() -> bool {
    // sysfs is the portable answer on Linux: every mains adapter exposes
    // `online`. No adapter at all means a desktop, which is never on battery.
    let Ok(entries) = std::fs::read_dir("/sys/class/power_supply") else {
        return false;
    };
    let mut saw_mains = false;
    for entry in entries.flatten() {
        let path = entry.path();
        let kind = std::fs::read_to_string(path.join("type")).unwrap_or_default();
        if kind.trim() != "Mains" {
            continue;
        }
        saw_mains = true;
        if std::fs::read_to_string(path.join("online"))
            .map(|value| value.trim() == "1")
            .unwrap_or(false)
        {
            return false;
        }
    }
    saw_mains
}

/// A cheap fingerprint of the currently mounted volumes.
///
/// Only used to detect *change*: when it differs from the last pass, a drive
/// was plugged in or pulled out and the window is told to re-scan. The value
/// itself is never shown.
fn volume_fingerprint() -> String {
    #[cfg(windows)]
    {
        use windows_sys::Win32::Storage::FileSystem::GetLogicalDrives;
        // A bitmask of present drive letters: one call, no allocation, and it
        // changes exactly when a volume appears or disappears.
        let mask = unsafe { GetLogicalDrives() };
        format!("{mask:08x}")
    }
    #[cfg(target_os = "macos")]
    {
        std::fs::read_dir("/Volumes")
            .map(|entries| {
                let mut names: Vec<String> = entries
                    .flatten()
                    .map(|e| e.file_name().to_string_lossy().into_owned())
                    .collect();
                names.sort();
                names.join("|")
            })
            .unwrap_or_default()
    }
    #[cfg(all(unix, not(target_os = "macos")))]
    {
        // /proc/mounts changes on every mount and unmount; its length and a
        // cheap sum are enough to notice, without parsing it here.
        std::fs::read_to_string("/proc/mounts")
            .map(|text| {
                let sum: u64 = text.bytes().map(u64::from).sum();
                format!("{}:{sum}", text.len())
            })
            .unwrap_or_default()
    }
}

/// A fingerprint of this machine's local addresses.
///
/// Changes when the machine joins a different network, which is the moment the
/// peer table needs rebuilding.
fn network_fingerprint() -> String {
    // Binding a UDP socket and asking for its local address reveals which
    // interface the routing table would use, with no packet sent and no
    // dependency. It answers "which network am I on" precisely enough to
    // detect a change.
    use std::net::UdpSocket;
    let Ok(socket) = UdpSocket::bind("0.0.0.0:0") else {
        return String::new();
    };
    if socket.connect("192.0.2.1:9").is_err() {
        // TEST-NET-1: routable in the table, never actually reachable.
        return String::new();
    }
    socket
        .local_addr()
        .map(|addr| addr.ip().to_string())
        .unwrap_or_default()
}

/// Starts the watcher thread.
pub fn start(app: AppHandle, state: Arc<AppState>) {
    std::thread::spawn(move || {
        let mut last_battery = on_battery();
        let mut last_volumes = volume_fingerprint();
        let mut last_network = network_fingerprint();
        state.on_battery.store(last_battery, Ordering::SeqCst);

        loop {
            std::thread::sleep(POLL);
            if state.shutting_down.load(Ordering::SeqCst) {
                return;
            }

            let battery = on_battery();
            if battery != last_battery {
                last_battery = battery;
                state.on_battery.store(battery, Ordering::SeqCst);
                emit(&app, SidecarEvent::PowerChanged { on_battery: battery });
            }

            let volumes = volume_fingerprint();
            if volumes != last_volumes {
                last_volumes = volumes;
                // The supervisor re-scans on its own housekeeping pass; this
                // makes the window ask for a fresh topology immediately rather
                // than up to fifteen seconds later.
                emit(&app, SidecarEvent::VolumesChanged);
            }

            let network = network_fingerprint();
            if network != last_network {
                last_network = network;
                emit(&app, SidecarEvent::NetworkChanged);
            }
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fingerprints_are_stable_between_calls() {
        // The point of a fingerprint is that it changes only when the thing it
        // describes changes; one that varied per call would fire a re-scan on
        // every poll.
        assert_eq!(volume_fingerprint(), volume_fingerprint());
        assert_eq!(network_fingerprint(), network_fingerprint());
    }

    #[test]
    fn reading_the_power_source_does_not_panic() {
        let _ = on_battery();
    }
}
