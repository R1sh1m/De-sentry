# Physical LAN Mesh Test Plan

## Overview

This document describes the manual verification steps for the **Add Device (invite‑pairing) flow** and the **Recovery Key UX** on three physical PCs. The goal is to ensure that:

* The host node correctly displays its LAN address, P2P port, and mesh code.
* A joining node can import these details, create a LAN‑Mesh node, and join the mesh.
* The recovery key is generated, exported, and persisted correctly.
* All nodes discover each other, replicate data, and reconstruct files.
* Firewall settings on Windows, macOS, and Linux allow the required ports.

## Prerequisites

1. **Three machines** (Windows 10/11, macOS 14, Linux Ubuntu 22.04) on the same LAN (wired or Wi‑Fi).
2. **De‑Sentry desktop app** built for the host OS (use the latest MSI/DMG or the development build).
3. **Ports open**:
   * API ports `7701‑7703` (one per node)
   * P2P ports `7801‑7803`
   * UDP discovery port `7901`
4. **Firewall configuration** – see `tests/physical/firewall_config.md`.
5. **Shared folder** (or removable drive) for each node’s data directory.
6. **Optional**: QR‑code scanner (not required for the current UI – the host shows address/port and mesh code as text).

## Test Steps

### 1. Prepare the Host Node
1. Launch De‑Sentry on **Machine A**.
2. Click **New node** → **Placement** → choose a folder.
3. In **Shape**, select **LAN Mesh Replica**.
4. Complete the wizard (steps 3‑5) – on step 5 the recovery key is shown.
5. **Export the recovery key** using **Save to a file…** and note the file location.
6. After creation, open the **Pair** sheet (mesh icon) and verify that the **Share this mesh** section displays:
   * **Host address and P2P port** – e.g. `192.168.1.10:7801`
   * **Mesh code** – a 64‑character hex string.
7. Record the displayed LAN address, P2P port, and mesh code.

### 2. Join from Machine B (and C)
1. Launch De‑Sentry on **Machine B**.
2. Click the **Pair** (mesh) button → **Join a LAN Mesh**.
3. Paste the **Host address and P2P port** from Machine A into the *Host address and P2P port* field.
4. Paste the **Mesh code** into the *Mesh code* field.
5. Click **Continue to create node** – the wizard opens with the bootstrap peers pre‑filled.
6. Complete the wizard (choose a data folder, optionally a passphrase, and export the recovery key if generated).
7. Repeat the same steps on **Machine C**.

### 3. Verify Mesh Connectivity
1. In each app, open the **Sidebar** – you should see the three nodes listed.
2. Verify that each node shows **Discovery: Enabled (UDP 7901)** in the *Review* step.
3. Open the **Explorer** on any node and upload a small file (e.g., a text file).
4. Switch to the other nodes and confirm the file appears with identical contents.
5. Verify that the **Topology** view shows all three nodes as peers.

### 4. Recovery Key Flow
1. For a node created with a **generated** recovery key, ensure the key was saved to a file.
2. Restart the node (close the app, then reopen and select the node).
3. Attempt to unlock the node – the app should prompt for the recovery key.
4. Provide the saved key file content; the node should start without error.
5. For a node created with a **passphrase**, verify that the *Recovery Key* step only shows the passphrase checkbox and that the node starts after confirming the checkbox.

### 5. Firewall Verification
1. On each machine, confirm that the firewall rules from `firewall_config.md` are applied.
2. Use `netstat -an` (Windows) / `lsof -i` (macOS/Linux) to ensure ports `770x`, `780x`, and `7901` are listening.
3. Attempt to ping the host’s LAN address from the other machines to verify basic network reachability.

## Expected Results
* All three nodes appear in the UI and can discover each other via UDP broadcast on port 7901.
* Data uploaded on any node replicates to the other two nodes.
* Recovery key export succeeds, and the node can be unlocked after a restart using the saved key.
* No firewall alerts or blocked connections; all required ports are open.

## Troubleshooting Tips
* **Discovery not working** – ensure UDP broadcast is not blocked; check that `7901` is open and that the LAN address is correct.
* **Port conflict** – if a port is already in use, the wizard will show an error; choose a different data folder or stop the conflicting process.
* **Recovery key missing** – the wizard will not allow finishing the flow without exporting or confirming the key; ensure the *I have safely stored this recovery key* checkbox is ticked.
* **Firewall still blocking** – double‑check OS‑specific firewall rules; some corporate networks may block UDP broadcast.

---

*Test plan created on $(date)*