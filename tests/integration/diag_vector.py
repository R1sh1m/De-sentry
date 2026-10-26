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

# Create vector collection
req('PUT', f'{BASE}/_schema/vec_test', {"type":"object","properties":{"embedding":{"type":"array"},"text":{"type":"string"}}})
# Insert vectors
for i in range(3):
    req('PUT', f'{BASE}/db/vec_test/doc_{i}', {"embedding":[float(i),float(i+1),float(i+2)],"text":f"doc {i}"})
# Bind to vector engine
print('BIND:', req('PUT', f'{BASE}/_collection/vec_test/engine', {"engine":"vector_hnsw_lite"}))
# Search
print('SEARCH:', req('POST', f'{BASE}/_search/vector/vec_test', {"query_vector":[0.5,1.5,2.5],"k":2}))
# Maybe query format is wrong? Try with vector instead of query_vector
print('SEARCH2:', req('POST', f'{BASE}/_search/vector/vec_test', {"vector":[0.5,1.5,2.5],"k":2}))