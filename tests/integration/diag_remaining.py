#!/usr/bin/env python3
import urllib.request, json
BASE = 'http://127.0.0.1:7701'
SUPER = 'http://127.0.0.1:7700'

def req(method, url, data=None):
    if data is not None:
        data = json.dumps(data).encode('utf-8')
    req = urllib.request.Request(url, data=data, method=method, headers={'Content-Type': 'application/json'})
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return 'OK', resp.read().decode()
    except urllib.error.HTTPError as e:
        body = e.read().decode() if hasattr(e, 'read') else ''
        return f'HTTP {e.code}', body
    except Exception as e:
        return 'ERR', str(e)

# Placement ring
print('PLACEMENT:', req('GET', f'{BASE}/_placement/tenant_data/tenant_abc/key123'))

# Manifest
print('MANIFEST:', req('POST', f'{SUPER}/_supervisor/manifest', {"node_id":"test_node","purpose":"test","quota_bytes":1073741824}))

# Also try GET on manifest
print('MANIFEST GET:', req('GET', f'{SUPER}/_supervisor/manifest'))
