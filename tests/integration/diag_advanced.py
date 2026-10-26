#!/usr/bin/env python3
import urllib.request, json
BASE = 'http://127.0.0.1:7701'

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

# 1. Retention run
req('PUT', f'{BASE}/_schema/ts_metrics', {"type":"object","properties":{"ts":{"type":"string"},"value":{"type":"number"}}})
req('PUT', f'{BASE}/_collection/ts_metrics/placement', {"shard_key":"metric_name","replication_factor":3,"retention_days":7})
for i in range(5):
    req('PUT', f'{BASE}/db/ts_metrics/metric_{i}', {"metric_name":"cpu","ts":f"2026-10-26T10:0{i}:00Z","value":42+i})
print('RETENTION:', req('POST', f'{BASE}/db/ts_metrics/_retention/run'))
print('ROLLUPS:', req('GET', f'{BASE}/_ts/ts_metrics/rollups'))

# 2. Vector
req('PUT', f'{BASE}/_schema/vec_docs', {"type":"object","properties":{"embedding":{"type":"array"},"text":{"type":"string"}}})
for i in range(3):
    req('PUT', f'{BASE}/db/vec_docs/doc_{i}', {"embedding":[float(i),float(i+1),float(i+2)],"text":f"doc {i}"})
print('VECTOR:', req('POST', f'{BASE}/_search/vector/vec_docs', {"query_vector":[0.5,1.5,2.5],"k":2}))

# 3. Graph
req('PUT', f'{BASE}/_schema/graph_nodes', {"type":"object","properties":{"name":{"type":"string"},"edges":{"type":"array"}}})
req('PUT', f'{BASE}/db/graph_nodes/A', {"name":"Node A","edges":["B","C"]})
print('GRAPH:', req('GET', f'{BASE}/_graph/graph_nodes/A'))

# 4. Columnar bind
req('PUT', f'{BASE}/_schema/col_data', {"type":"object","properties":{"id":{"type":"integer"},"val":{"type":"string"}}})
print('BIND:', req('PUT', f'{BASE}/_collection/col_data/engine', {"engine":"columnar_lite"}))

# 5. Placement ring
req('PUT', f'{BASE}/_schema/tenant_data', {"type":"object","properties":{"tenant":{"type":"string"},"data":{"type":"string"}}})
req('PUT', f'{BASE}/_collection/tenant_data/placement', {"shard_key":"tenant","replication_factor":3})
print('PLACEMENT:', req('GET', f'{BASE}/_placement/tenant_data/tenant_abc/key123'))

# 6. Manifest
print('MANIFEST:', req('POST', f'{BASE}/_supervisor/manifest', {"node_id":"test_node","purpose":"test","quota_bytes":1073741824}))