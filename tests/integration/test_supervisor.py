#!/usr/bin/env python3
import urllib.request, json
BASE = 'http://127.0.0.1:7700'
endpoints = [
    '/_supervisor/nodes',
    '/_supervisor/topology',
    '/_supervisor/scan',
    '/_status',
]
for ep in endpoints:
    url = BASE + ep
    try:
        req = urllib.request.Request(url, method='GET')
        with urllib.request.urlopen(req, timeout=10) as resp:
            body = resp.read().decode()
            print(f"{ep}: OK {len(body)} bytes")
    except urllib.error.HTTPError as e:
        print(f"{ep}: HTTPError {e.code} {e.reason}")
        try:
            print('Body:', e.read().decode())
        except Exception:
            pass
    except Exception as e:
        print(f"{ep}: Error {e}")
