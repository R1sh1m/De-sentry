//! Process supervision for `desentryd`.
//!
//! The app is the single point of access: nobody runs `desentryd` by hand and
//! nobody edits node.json. This module is what makes that true. It spawns each
//! node with `--config <path>`, captures both output streams, watches for
//! exits, restarts crashes with backoff, and reports every state change to the
//! window.
//!
//! Three decisions worth stating:
//!
//!   * **A node is not "started" until it answers.** Spawning returns a PID,
//!     which proves nothing -- a node whose port is taken exits a moment later.
//!     `spawn` polls `GET /_status` until the node replies or the deadline
//!     passes, and only then does it have a node_id to key the node by. The
//!     window therefore never sees a node under a provisional id that later
//!     changes underneath it.
//!
//!   * **Restarts back off and then stop.** A node that crashes on startup --
//!     a corrupt data file, a port that is genuinely taken -- would otherwise
//!     be restarted forever, burning CPU and filling the log with the same
//!     failure. After `MAX_RESTARTS` inside `RESTART_WINDOW` the supervisor
//!     leaves it stopped and says why.
//!
//!   * **Stopping is polite first.** Nodes are asked to exit and only killed
//!     if they do not; a node killed mid-write leaves its WAL to be recovered
//!     on next start, which works but is not something to do routinely.

use std::collections::VecDeque;
use std::io::{BufRead, BufReader, Write};
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};

use crate::http;

/// How long a node has to answer `/_status` before it counts as failed to start.
const READY_TIMEOUT: Duration = Duration::from_secs(20);
/// How often the readiness probe retries inside that window.
const READY_POLL: Duration = Duration::from_millis(150);
/// Restart budget, and the window it is measured over.
const MAX_RESTARTS: u32 = 5;
const RESTART_WINDOW: Duration = Duration::from_secs(120);
/// Grace period between asking a node to stop and killing it. Unix-only:
/// Windows has no SIGTERM for console-less children, so `stop()` there goes
/// straight to kill and this constant would be unused (hence the gate rather
/// than an allow).
#[cfg(unix)]
const STOP_GRACE: Duration = Duration::from_secs(5);
/// Captured log lines kept per node. Enough to see a startup failure in full.
const LOG_CAPACITY: usize = 2000;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum ProcessState {
    Starting,
    Running,
    Restarting,
    Stopped,
    Failed,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct LogLine {
    pub ts_ms: i64,
    pub stream: &'static str,
    pub line: String,
}

/// The view of a node the window sees. Mirrors `SupervisedNode` in bridge.ts.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SupervisedNode {
    pub node_id: String,
    pub node_name: String,
    pub data_dir: String,
    pub config_path: String,
    pub api_port: u16,
    pub p2p_port: u16,
    pub discovery_port: u16,
    pub supervisor: bool,
    pub process: ProcessState,
    pub pid: Option<u32>,
    pub restarts: u32,
    pub last_exit_code: Option<i32>,
    pub last_error: String,
    pub started_ms: i64,
    pub removable: bool,
    pub encrypted: bool,
}

#[derive(Debug, thiserror::Error)]
pub enum NodeError {
    #[error("could not start desentryd: {0}")]
    Spawn(String),
    #[error("{0} did not answer on 127.0.0.1:{1} within {2} seconds. Its log ends with: {3}")]
    NotReady(String, u16, u64, String),
    #[error("no node with id {0}")]
    Unknown(String),
    #[error("{0}")]
    Io(String),
}

pub fn now_ms() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0)
}

/// What a node needs to be launched. Everything durable is already on disk.
#[derive(Debug, Clone)]
pub struct LaunchSpec {
    pub node_name: String,
    pub data_dir: PathBuf,
    pub config_path: PathBuf,
    pub api_port: u16,
    pub p2p_port: u16,
    pub discovery_port: u16,
    pub supervisor: bool,
    pub removable: bool,
    pub encrypted: bool,
    /// Written to the child's stdin and immediately dropped. Never logged,
    /// never stored, never passed as an argument -- an argument is visible in
    /// every process listing on the machine.
    pub unlock_secret: Option<String>,
}

/// One running node: the child process, its logs, and its reported state.
pub struct NodeHandle {
    pub spec: LaunchSpec,
    pub node_id: String,
    pub state: ProcessState,
    pub pid: Option<u32>,
    pub restarts: u32,
    pub last_exit_code: Option<i32>,
    pub last_error: String,
    pub started_ms: i64,
    /// Restart timestamps inside the window, for the budget.
    restart_times: VecDeque<Instant>,
    child: Option<Child>,
    logs: Arc<Mutex<VecDeque<LogLine>>>,
    /// Set when a stop was asked for, so the exit watcher does not restart it.
    stopping: Arc<AtomicBool>,
}

impl NodeHandle {
    pub fn view(&self) -> SupervisedNode {
        SupervisedNode {
            node_id: self.node_id.clone(),
            node_name: self.spec.node_name.clone(),
            data_dir: self.spec.data_dir.to_string_lossy().into_owned(),
            config_path: self.spec.config_path.to_string_lossy().into_owned(),
            api_port: self.spec.api_port,
            p2p_port: self.spec.p2p_port,
            discovery_port: self.spec.discovery_port,
            supervisor: self.spec.supervisor,
            process: self.state,
            pid: self.pid,
            restarts: self.restarts,
            last_exit_code: self.last_exit_code,
            last_error: self.last_error.clone(),
            started_ms: self.started_ms,
            removable: self.spec.removable,
            encrypted: self.spec.encrypted,
        }
    }

    pub fn logs(&self, tail: usize) -> Vec<LogLine> {
        let logs = self.logs.lock().expect("log mutex");
        let skip = logs.len().saturating_sub(tail);
        logs.iter().skip(skip).cloned().collect()
    }

    /// The last few lines, for an error message. A failure the user cannot see
    /// the reason for is a failure they will file a bug about.
    fn log_tail(&self, lines: usize) -> String {
        self.logs(lines)
            .iter()
            .map(|entry| entry.line.as_str())
            .collect::<Vec<_>>()
            .join(" | ")
    }

    /// True when the restart budget still has room.
    fn may_restart(&mut self) -> bool {
        // `checked_sub` rather than `-`: subtracting from an Instant taken
        // shortly after boot underflows and panics on some platforms.
        if let Some(cutoff) = Instant::now().checked_sub(RESTART_WINDOW) {
            while self.restart_times.front().is_some_and(|when| *when < cutoff) {
                self.restart_times.pop_front();
            }
        }
        self.restart_times.len() < MAX_RESTARTS as usize
    }

    fn note_restart(&mut self) {
        self.restart_times.push_back(Instant::now());
        self.restarts += 1;
    }
}

/// Spawns `desentryd` and captures its output. Does not wait for readiness.
fn spawn_child(
    binary: &PathBuf,
    spec: &LaunchSpec,
    logs: Arc<Mutex<VecDeque<LogLine>>>,
) -> Result<Child, NodeError> {
    let mut command = Command::new(binary);
    command
        .arg("--config")
        .arg(&spec.config_path)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());

    // Nodes inherit no environment surprises: the engine reads its config file
    // and nothing else, and an inherited variable that changed behaviour would
    // be invisible in the app.
    command.env_remove("DESENTRY_CONFIG");

    #[cfg(windows)]
    {
        // Without this every node start flashes a console window.
        use std::os::windows::process::CommandExt;
        const CREATE_NO_WINDOW: u32 = 0x0800_0000;
        command.creation_flags(CREATE_NO_WINDOW);
    }

    let mut child = command
        .spawn()
        .map_err(|error| NodeError::Spawn(format!("{}: {error}", binary.display())))?;

    if let Some(secret) = spec.unlock_secret.as_ref() {
        if let Some(stdin) = child.stdin.as_mut() {
            // One line, then the pipe closes. The engine reads the unlock key
            // from stdin precisely so it never appears in a config file, a
            // command line or an environment variable.
            let _ = stdin.write_all(secret.as_bytes());
            let _ = stdin.write_all(b"\n");
            let _ = stdin.flush();
        }
    }
    drop(child.stdin.take());

    if let Some(stdout) = child.stdout.take() {
        pump(stdout, "stdout", Arc::clone(&logs));
    }
    if let Some(stderr) = child.stderr.take() {
        pump(stderr, "stderr", Arc::clone(&logs));
    }
    Ok(child)
}

/// Reads a child stream into the ring buffer on its own thread.
fn pump<R: std::io::Read + Send + 'static>(
    stream: R,
    label: &'static str,
    logs: Arc<Mutex<VecDeque<LogLine>>>,
) {
    std::thread::spawn(move || {
        let reader = BufReader::new(stream);
        for line in reader.lines() {
            let Ok(line) = line else { break };
            let mut logs = match logs.lock() {
                Ok(logs) => logs,
                // A poisoned log mutex must not take the reader thread with it:
                // losing log lines is survivable, losing the reader means the
                // child's pipe fills and it blocks on write.
                Err(poisoned) => poisoned.into_inner(),
            };
            if logs.len() >= LOG_CAPACITY {
                logs.pop_front();
            }
            logs.push_back(LogLine {
                ts_ms: now_ms(),
                stream: label,
                line,
            });
        }
    });
}

/// Polls `/_status` until the node answers, and returns its reported node_id.
fn wait_until_ready(port: u16, deadline: Instant) -> Option<(String, String)> {
    while Instant::now() < deadline {
        if let Ok(status) = http::status(port, Duration::from_millis(750)) {
            let node_id = status.get("node_id").and_then(|v| v.as_str()).unwrap_or("");
            let node_name = status.get("node_name").and_then(|v| v.as_str()).unwrap_or("");
            if !node_id.is_empty() {
                return Some((node_id.to_owned(), node_name.to_owned()));
            }
        }
        std::thread::sleep(READY_POLL);
    }
    None
}

/// Starts a node and waits for it to answer.
///
/// On failure the child is killed rather than left running: a node that did not
/// come up but is still holding a port is worse than one that is not there.
pub fn start(binary: &PathBuf, spec: LaunchSpec) -> Result<NodeHandle, NodeError> {
    let logs: Arc<Mutex<VecDeque<LogLine>>> = Arc::new(Mutex::new(VecDeque::new()));
    let mut child = spawn_child(binary, &spec, Arc::clone(&logs))?;
    let pid = child.id();

    let mut handle = NodeHandle {
        node_id: String::new(),
        state: ProcessState::Starting,
        pid: Some(pid),
        restarts: 0,
        last_exit_code: None,
        last_error: String::new(),
        started_ms: now_ms(),
        restart_times: VecDeque::new(),
        child: None,
        logs: Arc::clone(&logs),
        stopping: Arc::new(AtomicBool::new(false)),
        spec,
    };

    match wait_until_ready(handle.spec.api_port, Instant::now() + READY_TIMEOUT) {
        Some((node_id, node_name)) => {
            handle.node_id = node_id;
            if handle.spec.node_name.is_empty() && !node_name.is_empty() {
                handle.spec.node_name = node_name;
            }
            handle.state = ProcessState::Running;
            handle.child = Some(child);
            Ok(handle)
        }
        None => {
            let tail = handle.log_tail(6);
            let _ = child.kill();
            let _ = child.wait();
            Err(NodeError::NotReady(
                handle.spec.node_name.clone(),
                handle.spec.api_port,
                READY_TIMEOUT.as_secs(),
                if tail.is_empty() {
                    "(it produced no output)".to_owned()
                } else {
                    tail
                },
            ))
        }
    }
}

/// Asks a node to stop, then kills it if it does not.
pub fn stop(handle: &mut NodeHandle) {
    handle.stopping.store(true, Ordering::SeqCst);
    let Some(child) = handle.child.as_mut() else {
        handle.state = ProcessState::Stopped;
        handle.pid = None;
        return;
    };

    // On Unix a SIGTERM lets `desentryd` flush and close cleanly. Windows has
    // no equivalent for a console-less child, so the kill is the graceful path
    // there -- the WAL exists for exactly this.
    #[cfg(unix)]
    {
        let pid = child.id() as libc::pid_t;
        // SAFETY: `pid` is this process's own child, and SIGTERM to a live or
        // already-reaped pid is harmless -- the failure mode is ESRCH, which
        // the grace loop below handles by falling through to kill().
        unsafe {
            libc::kill(pid, libc::SIGTERM);
        }
        let deadline = Instant::now() + STOP_GRACE;
        while Instant::now() < deadline {
            if matches!(child.try_wait(), Ok(Some(_))) {
                break;
            }
            std::thread::sleep(Duration::from_millis(100));
        }
    }

    let _ = child.kill();
    let status = child.wait().ok();
    handle.last_exit_code = status.and_then(|s| s.code());
    handle.state = ProcessState::Stopped;
    handle.pid = None;
    handle.child = None;
}

/// Checks whether a node has exited, and whether it should be restarted.
///
/// Returns `Some(exit_code)` when the process ended since the last check. The
/// caller decides what to do; this only reports.
pub fn poll_exit(handle: &mut NodeHandle) -> Option<Option<i32>> {
    let child = handle.child.as_mut()?;
    match child.try_wait() {
        Ok(Some(status)) => {
            let code = status.code();
            handle.last_exit_code = code;
            handle.pid = None;
            handle.child = None;
            if handle.stopping.load(Ordering::SeqCst) {
                handle.state = ProcessState::Stopped;
            } else {
                handle.state = ProcessState::Failed;
                handle.last_error = match code {
                    Some(0) => "the node exited on its own".to_owned(),
                    Some(code) => format!("exited with code {code}: {}", handle.log_tail(4)),
                    None => format!("killed by a signal: {}", handle.log_tail(4)),
                };
            }
            Some(code)
        }
        Ok(None) => None,
        Err(error) => {
            handle.last_error = format!("could not check on the process: {error}");
            handle.state = ProcessState::Failed;
            handle.child = None;
            Some(None)
        }
    }
}

/// Restarts a node that has exited, if its budget allows.
pub fn restart(binary: &PathBuf, handle: &mut NodeHandle) -> Result<(), NodeError> {
    if !handle.may_restart() {
        handle.state = ProcessState::Failed;
        handle.last_error = format!(
            "stopped after {MAX_RESTARTS} restarts in {} seconds. The last failure was: {}",
            RESTART_WINDOW.as_secs(),
            handle.log_tail(4)
        );
        return Err(NodeError::Io(handle.last_error.clone()));
    }

    handle.note_restart();
    handle.state = ProcessState::Restarting;
    handle.stopping.store(false, Ordering::SeqCst);

    // Backoff grows with consecutive restarts so a node failing for a reason
    // that will not resolve is not retried every 200ms.
    let backoff = Duration::from_millis(250 * (1 << handle.restart_times.len().min(5)) as u64);
    std::thread::sleep(backoff);

    let logs = Arc::clone(&handle.logs);
    let mut child = spawn_child(binary, &handle.spec, logs)?;
    handle.pid = Some(child.id());
    handle.started_ms = now_ms();

    match wait_until_ready(handle.spec.api_port, Instant::now() + READY_TIMEOUT) {
        Some((node_id, _)) => {
            // A restart that comes back with a different identity means the
            // data directory was replaced underneath us. Keeping the old id
            // would attach the window's selection to a node that no longer
            // exists.
            handle.node_id = node_id;
            handle.state = ProcessState::Running;
            handle.last_error.clear();
            handle.child = Some(child);
            Ok(())
        }
        None => {
            let tail = handle.log_tail(6);
            let _ = child.kill();
            let _ = child.wait();
            handle.state = ProcessState::Failed;
            handle.last_error = format!("did not answer after restarting: {tail}");
            handle.child = None;
            handle.pid = None;
            Err(NodeError::NotReady(
                handle.spec.node_name.clone(),
                handle.spec.api_port,
                READY_TIMEOUT.as_secs(),
                tail,
            ))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn handle() -> NodeHandle {
        NodeHandle {
            spec: LaunchSpec {
                node_name: "test".into(),
                data_dir: PathBuf::from("/tmp/x"),
                config_path: PathBuf::from("/tmp/x/node.json"),
                api_port: 7799,
                p2p_port: 7899,
                discovery_port: 7901,
                supervisor: false,
                removable: false,
                encrypted: false,
                unlock_secret: None,
            },
            node_id: "abc".into(),
            state: ProcessState::Running,
            pid: Some(1),
            restarts: 0,
            last_exit_code: None,
            last_error: String::new(),
            started_ms: 0,
            restart_times: VecDeque::new(),
            child: None,
            logs: Arc::new(Mutex::new(VecDeque::new())),
            stopping: Arc::new(AtomicBool::new(false)),
        }
    }

    #[test]
    fn the_restart_budget_runs_out() {
        let mut node = handle();
        for _ in 0..MAX_RESTARTS {
            assert!(node.may_restart());
            node.note_restart();
        }
        assert!(!node.may_restart());
        assert_eq!(node.restarts, MAX_RESTARTS);
    }

    #[test]
    fn logs_are_bounded() {
        let node = handle();
        {
            let mut logs = node.logs.lock().unwrap();
            for i in 0..(LOG_CAPACITY + 500) {
                if logs.len() >= LOG_CAPACITY {
                    logs.pop_front();
                }
                logs.push_back(LogLine {
                    ts_ms: 0,
                    stream: "stdout",
                    line: format!("line {i}"),
                });
            }
        }
        assert_eq!(node.logs(LOG_CAPACITY * 2).len(), LOG_CAPACITY);
        // The newest lines are the ones kept: a startup failure scrolling out
        // of a ring buffer that kept the oldest lines would be useless.
        assert!(node.logs(1)[0].line.ends_with(&format!("{}", LOG_CAPACITY + 499)));
    }

    #[test]
    fn the_view_carries_no_secret() {
        let mut node = handle();
        node.spec.unlock_secret = Some("hunter2".into());
        let json = serde_json::to_string(&node.view()).unwrap();
        assert!(!json.contains("hunter2"));
    }
}
