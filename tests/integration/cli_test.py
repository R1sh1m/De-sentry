#!/usr/bin/env python3
"""
CLI integration test for desentry_cli.
It verifies that the binary runs, shows help, and can query node status.
"""
import subprocess, sys, os, time

# Helper to run a command and capture output
def run(cmd, timeout=30):
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, check=False)
        return result.returncode, result.stdout.strip(), result.stderr.strip()
    except Exception as e:
        return -1, "", str(e)

def log(msg, level="INFO"):
    ts = time.strftime("%H:%M:%S")
    print(f"[{ts}] [{level}] {msg}")

def main():
    # Determine binary path (built in build/RelWithDebInfo or build/)
    candidates = [
        os.path.abspath(os.path.join('build', 'RelWithDebInfo', 'desentry_cli.exe')),
        os.path.abspath(os.path.join('build', 'desentry_cli.exe')),
        os.path.abspath(os.path.join('build', 'Debug', 'desentry_cli.exe')),
        os.path.abspath(os.path.join('build', 'Release', 'desentry_cli.exe')),
    ]
    bin_path = None
    for p in candidates:
        if os.path.exists(p):
            bin_path = p
            break
    if not bin_path:
        log(f"CLI binary not found. Looked in: {candidates}", "FAIL")
        sys.exit(1)
    log(f"Using CLI binary: {bin_path}")
    # 1. Help
    rc, out, err = run([bin_path, '--help'])
    if rc == 0 or ('usage' in out.lower()):
        log("CLI help OK")
    else:
        log(f"CLI help failed rc={rc} err={err}", "FAIL")
        sys.exit(1)
    # 2. Status (should hit http://127.0.0.1:7701/_status by default)
    rc, out, err = run([bin_path, 'status'])
    if rc == 0 and 'node_id' in out:
        log("CLI status OK")
    else:
        log(f"CLI status failed rc={rc} out={out} err={err}", "FAIL")
        sys.exit(1)
    sys.exit(0)

if __name__ == '__main__':
    main()
