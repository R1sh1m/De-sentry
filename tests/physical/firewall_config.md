# Firewall Configuration for De‑Sentry LAN Mesh

The De‑Sentry mesh requires three sets of ports to be reachable between all participating machines:

* **API ports** – `7701`, `7702`, `7703` (TCP)
* **P2P ports** – `7801`, `7802`, `7803` (TCP)
* **Discovery port** – `7901` (UDP broadcast)

Below are the minimal firewall rules for Windows Defender Firewall, macOS PF, and Linux `iptables`/`nftables`.

---

## Windows 10/11 – Windows Defender Firewall

1. Open **Windows Defender Firewall with Advanced Security**.
2. Create a **New Inbound Rule** for each protocol:
   * **Protocol**: TCP
   * **Local port**: `7701-7703, 7801-7803`
   * **Action**: Allow the connection
   * **Profile**: Domain, Private, Public (as appropriate for your network)
3. Create a **New Inbound Rule** for UDP discovery:
   * **Protocol**: UDP
   * **Local port**: `7901`
   * **Action**: Allow the connection
   * **Profile**: Domain, Private, Public
4. (Optional) Restrict the rule to the specific network interface used for the LAN.

## macOS – PF (Packet Filter)

Edit `/etc/pf.conf` (requires sudo) and add the following lines, then reload PF:

```pf
# Allow TCP API and P2P ports
pass in proto tcp from any to any port {7701, 7702, 7703, 7801, 7802, 7803} keep state

# Allow UDP discovery broadcast on port 7901
pass in proto udp from any to any port 7901 keep state
```

Reload PF:

```bash
sudo pfctl -f /etc/pf.conf
sudo pfctl -e   # enable if not already enabled
```

## Linux – iptables (or nftables)

### iptables
```bash
# Allow TCP API and P2P ports
sudo iptables -A INPUT -p tcp -m multiport --dports 7701,7702,7703,7801,7802,7803 -j ACCEPT

# Allow UDP discovery broadcast (port 7901)
sudo iptables -A INPUT -p udp --dport 7901 -j ACCEPT

# (Optional) Restrict to the LAN interface, e.g., eth0
# sudo iptables -A INPUT -i eth0 -p tcp -m multiport --dports 7701,7702,7703,7801,7802,7803 -j ACCEPT
# sudo iptables -A INPUT -i eth0 -p udp --dport 7901 -j ACCEPT
```

### nftables (modern alternative)
```bash
sudo nft add rule inet filter input tcp dport {7701,7702,7703,7801,7802,7803} accept
sudo nft add rule inet filter input udp dport 7901 accept
```

---

**Verification**

After applying the rules, run on each machine:

```bash
# Windows (PowerShell)
netstat -an | findstr "7701"
netstat -an | findstr "7801"
netstat -an | findstr "7901"

# macOS / Linux
sudo lsof -iTCP -sTCP:LISTEN -P | grep -E "770[1-3]|780[1-3]"
sudo lsof -iUDP -P | grep 7901
```

All listed ports should be in the **LISTEN** state.

---

*Created on $(date)*