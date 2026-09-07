//! A minimal loopback HTTP client.
//!
//! The sidecar needs exactly one thing over HTTP: ask a freshly spawned node
//! `GET /_status` on 127.0.0.1 and read its `node_id`. That is one request, no
//! TLS, no redirects, no chunked encoding worth speaking of, and no keep-alive
//! -- so it is forty lines here rather than a dependency and its transitive
//! tree.
//!
//! The web view does its own HTTP with `fetch`; this exists only for readiness
//! probes on the Rust side.

use std::io::{Read, Write};
use std::net::{Shutdown, SocketAddr, TcpStream};
use std::time::Duration;

#[derive(Debug, thiserror::Error)]
pub enum HttpError {
    #[error("could not connect to 127.0.0.1:{0}: {1}")]
    Connect(u16, std::io::Error),
    #[error("transport error talking to 127.0.0.1:{0}: {1}")]
    Io(u16, std::io::Error),
    #[error("malformed response from 127.0.0.1:{0}")]
    Malformed(u16),
    #[error("127.0.0.1:{0} answered HTTP {1}")]
    Status(u16, u16),
}

/// `GET path` against a node's loopback API. Returns the response body.
pub fn get(port: u16, path: &str, timeout: Duration) -> Result<String, HttpError> {
    let address = SocketAddr::from(([127, 0, 0, 1], port));
    let mut stream =
        TcpStream::connect_timeout(&address, timeout).map_err(|error| HttpError::Connect(port, error))?;
    stream
        .set_read_timeout(Some(timeout))
        .map_err(|error| HttpError::Io(port, error))?;
    stream
        .set_write_timeout(Some(timeout))
        .map_err(|error| HttpError::Io(port, error))?;

    // `Connection: close` is what lets the read loop end at EOF instead of
    // needing to parse Content-Length to know when to stop.
    let request = format!(
        "GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUser-Agent: de-sentry-app\r\nConnection: close\r\nAccept: application/json\r\n\r\n"
    );
    stream
        .write_all(request.as_bytes())
        .map_err(|error| HttpError::Io(port, error))?;
    let _ = stream.shutdown(Shutdown::Write);

    let mut raw = Vec::new();
    stream
        .read_to_end(&mut raw)
        .map_err(|error| HttpError::Io(port, error))?;

    let text = String::from_utf8_lossy(&raw).into_owned();
    let (head, body) = text
        .split_once("\r\n\r\n")
        .or_else(|| text.split_once("\n\n"))
        .ok_or(HttpError::Malformed(port))?;

    let status: u16 = head
        .lines()
        .next()
        .and_then(|line| line.split_whitespace().nth(1))
        .and_then(|code| code.parse().ok())
        .ok_or(HttpError::Malformed(port))?;
    if !(200..300).contains(&status) {
        return Err(HttpError::Status(port, status));
    }
    Ok(body.to_owned())
}

/// `GET /_status`, parsed. Used as the readiness probe and to learn a node's id.
pub fn status(port: u16, timeout: Duration) -> Result<serde_json::Value, HttpError> {
    let body = get(port, "/_status", timeout)?;
    serde_json::from_str(&body).map_err(|_| HttpError::Malformed(port))
}
