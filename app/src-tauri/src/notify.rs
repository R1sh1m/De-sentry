//! OS notifications.
//!
//! Only three things are worth interrupting someone for, and they are all
//! things that happened while they were not looking:
//!
//!   * a node stopped and is not coming back,
//!   * writes held for a node while it was away have been claimed by it,
//!   * a checkpoint pruned history.
//!
//! Everything else -- a peer appearing, a write landing, a gossip round -- is
//! visible in the window when the window is open and is not news when it is
//! not. A control room that notifies on every event trains people to dismiss
//! its notifications, which costs exactly the three above.

use tauri::AppHandle;
use tauri_plugin_notification::NotificationExt;

use crate::nodes::SupervisedNode;

fn send(app: &AppHandle, title: &str, body: &str) {
    // Notification permission can be refused, and the platform can be between
    // states during shutdown. A notification that cannot be shown is not a
    // reason to fail whatever produced it.
    let _ = app
        .notification()
        .builder()
        .title(title)
        .body(body)
        .show();
}

/// A node exited and the supervisor is not bringing it back.
pub fn node_stopped(app: &AppHandle, node: &SupervisedNode) {
    let name = if node.node_name.is_empty() {
        node.node_id.chars().take(8).collect::<String>()
    } else {
        node.node_name.clone()
    };

    let detail = if node.last_error.is_empty() {
        match node.last_exit_code {
            Some(code) => format!("It exited with code {code}."),
            None => "It exited unexpectedly.".to_owned(),
        }
    } else {
        node.last_error.clone()
    };

    send(
        app,
        &format!("{name} stopped"),
        &format!("{detail} Its replicas will hold any writes addressed to it until it returns."),
    );
}

/// A node came back and pulled the writes its replicas were holding.
pub fn transit_claimed(app: &AppHandle, node_name: &str, documents: u64) {
    if documents == 0 {
        return;
    }
    send(
        app,
        &format!("{node_name} caught up"),
        &format!(
            "It claimed {documents} document{} that were written while it was offline.",
            if documents == 1 { "" } else { "s" }
        ),
    );
}

/// A checkpoint pruned ledger history after a quorum agreed on the tip.
pub fn checkpoint(app: &AppHandle, entries_pruned: u64, agreeing: u64, required: u64) {
    if entries_pruned == 0 {
        return;
    }
    send(
        app,
        "History checkpointed",
        &format!(
            "{entries_pruned} ledger entries were pruned after {agreeing} of {required} replicas agreed on the same tip."
        ),
    );
}
