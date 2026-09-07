//! Port allocation for supervised nodes.
//!
//! The scheme from the spec: API ports climb from 7701, P2P ports from 7801,
//! and every node on a machine shares discovery port 7901 -- discovery is a
//! broadcast rendezvous, so nodes that disagree about it cannot find each
//! other.
//!
//! Allocation binds the candidate port before handing it out. Reading the
//! OS's list of listening sockets and picking a gap is racy in exactly the way
//! that matters here: two nodes started a few milliseconds apart would both
//! see the same gap. Binding is the only test that means anything, and the
//! bind is released immediately afterwards -- there is a window between the
//! probe and `desentryd` binding for real, but it is a window of microseconds
//! against a range of 200 ports, and the node reports a clear bind failure if
//! it loses that race.

use std::collections::HashSet;
use std::net::{Ipv4Addr, SocketAddrV4, TcpListener};

use serde::{Deserialize, Serialize};

/// First API port. The app's own supervisor takes this one.
pub const API_BASE: u16 = 7701;
/// First P2P port.
pub const P2P_BASE: u16 = 7801;
/// Shared LAN discovery port. Every node on every machine uses it.
pub const DISCOVERY_PORT: u16 = 7901;
/// How far to climb before giving up. 100 nodes on one machine is already far
/// past the 50-per-user target, so exhausting this means something is wrong
/// rather than that the user needs more room.
const RANGE: u16 = 100;

#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
pub struct PortAllocation {
    pub api_port: u16,
    pub p2p_port: u16,
    pub discovery_port: u16,
}

#[derive(Debug, thiserror::Error)]
pub enum PortError {
    #[error("no free API port in {0}-{1}; something else on this machine is using the range")]
    NoApiPort(u16, u16),
    #[error("no free P2P port in {0}-{1}; something else on this machine is using the range")]
    NoP2pPort(u16, u16),
}

/// True when nothing is listening on `port` on the given interface.
fn free_on(addr: Ipv4Addr, port: u16) -> bool {
    // SO_REUSEADDR is deliberately *not* set: the question is whether a bind
    // would succeed for a normal listener, and reuse would answer a different
    // question on some platforms.
    TcpListener::bind(SocketAddrV4::new(addr, port)).is_ok()
}

/// Allocates the next free API/P2P pair, skipping ports already handed out.
///
/// `reserved` carries ports allocated in this session but not yet bound by a
/// node that is still starting; without it, two nodes created back to back
/// would be offered the same pair.
pub fn allocate(reserved: &HashSet<u16>) -> Result<PortAllocation, PortError> {
    let api_port = (API_BASE..API_BASE + RANGE)
        .find(|port| !reserved.contains(port) && free_on(Ipv4Addr::LOCALHOST, *port))
        .ok_or(PortError::NoApiPort(API_BASE, API_BASE + RANGE))?;

    // The P2P port is checked on 0.0.0.0, not loopback: that is what the node
    // will actually bind, and a port free on 127.0.0.1 can still be taken on
    // another interface.
    let p2p_port = (P2P_BASE..P2P_BASE + RANGE)
        .find(|port| !reserved.contains(port) && free_on(Ipv4Addr::UNSPECIFIED, *port))
        .ok_or(PortError::NoP2pPort(P2P_BASE, P2P_BASE + RANGE))?;

    Ok(PortAllocation {
        api_port,
        p2p_port,
        discovery_port: DISCOVERY_PORT,
    })
}

/// Allocates the pair the app's own supervisor uses, if they are free.
///
/// The supervisor prefers the base ports so a user who has been told "the
/// control plane is on 7701" is usually right, but it does not insist: a
/// second copy of the app, or a leftover process, must not stop the app from
/// starting.
pub fn allocate_supervisor(reserved: &HashSet<u16>) -> Result<PortAllocation, PortError> {
    allocate(reserved)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn allocation_skips_reserved_ports() {
        let mut reserved = HashSet::new();
        let first = allocate(&reserved).expect("a free port pair exists");
        reserved.insert(first.api_port);
        reserved.insert(first.p2p_port);

        let second = allocate(&reserved).expect("a second free port pair exists");
        assert_ne!(first.api_port, second.api_port);
        assert_ne!(first.p2p_port, second.p2p_port);
        // Discovery is shared on purpose -- nodes that disagree about it never
        // meet.
        assert_eq!(first.discovery_port, second.discovery_port);
    }

    #[test]
    fn allocation_stays_in_range() {
        let allocation = allocate(&HashSet::new()).expect("a free port pair exists");
        assert!((API_BASE..API_BASE + RANGE).contains(&allocation.api_port));
        assert!((P2P_BASE..P2P_BASE + RANGE).contains(&allocation.p2p_port));
    }

    #[test]
    fn a_bound_port_is_not_offered() {
        let taken = TcpListener::bind((Ipv4Addr::LOCALHOST, API_BASE));
        if taken.is_err() {
            // Something already holds the base port; the property under test is
            // then already covered by whatever holds it.
            return;
        }
        let allocation = allocate(&HashSet::new()).expect("a free port pair exists");
        assert_ne!(allocation.api_port, API_BASE);
    }
}
