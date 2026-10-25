#!/usr/bin/env python3
import urllib.request, json, time

def put(host, port, coll, key, body):
    data = json.dumps(body).encode()
    req = urllib.request.Request(f'http://{host}:{port}/db/{coll}/{key}', data=data, method='PUT', headers={'Content-Type':'application/json'})
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.read().decode()
    except Exception as e:
        return f'ERR: {e}'

def lookup(host, port, collection, query):
    try:
        with urllib.request.urlopen(f'http://{host}:{port}/_lookup/{collection}?query={query}', timeout=5) as r:
            return json.loads(r.read().decode())
    except Exception as e:
        return f'ERR: {e}'

# Add test data
print('Adding test data...')
print(put('127.0.0.1', 7701, 'users', 'user_001', {'name': 'Alice', 'role': 'dev'}))
print(put('127.0.0.1', 7701, 'users', 'user_002', {'name': 'Bob', 'role': 'admin'}))
print(put('127.0.0.1', 7701, 'users', 'user_003', {'name': 'Charlie', 'role': 'dev'}))
print(put('127.0.0.1', 7701, 'notes', 'note_001', {'title': 'Meeting notes', 'content': 'Discussed project roadmap'}))
print(put('127.0.0.1', 7701, 'notes', 'note_002', {'title': 'Design review', 'content': 'UI feedback incorporated'}))
time.sleep(2)

# Test lookup
print('\nTesting lookup...')
print('Query "user":', json.dumps(lookup('127.0.0.1', 7701, '*', 'user'), indent=2))
print('Query "Alice":', json.dumps(lookup('127.0.0.1', 7701, '*', 'Alice'), indent=2))
print('Query "notes":', json.dumps(lookup('127.0.0.1', 7701, '*', 'notes'), indent=2))