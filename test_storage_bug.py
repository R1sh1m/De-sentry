#!/usr/bin/env python3
import urllib.request, json, time

def put(host, port, coll, key, body):
    data = json.dumps(body).encode()
    req = urllib.request.Request(f'http://{host}:{port}/db/{coll}/{key}', data=data, method='PUT', headers={'Content-Type':'application/json'})
    with urllib.request.urlopen(req, timeout=5) as r:
        return json.loads(r.read().decode())

def get_collection(host, port, coll):
    with urllib.request.urlopen(f'http://{host}:{port}/db/{coll}', timeout=5) as r:
        return json.loads(r.read().decode())

# Add data to users collection
print('Adding to users...')
put('127.0.0.1', 7701, 'users', 'u1', {'name': 'Alice'})
put('127.0.0.1', 7701, 'users', 'u2', {'name': 'Bob'})
time.sleep(1)

# Add data to notes collection  
print('Adding to notes...')
put('127.0.0.1', 7701, 'notes', 'n1', {'title': 'Note 1'})
put('127.0.0.1', 7701, 'notes', 'n2', {'title': 'Note 2'})
time.sleep(1)

# Check users
print('\nUsers collection:')
users = get_collection('127.0.0.1', 7701, 'users')
print('  count:', users['count'])
for doc in users['documents']:
    print('   ', doc['key'], ':', doc['document'])

# Check notes
print('\nNotes collection:')
notes = get_collection('127.0.0.1', 7701, 'notes')
print('  count:', notes['count'])
for doc in notes['documents']:
    print('   ', doc['key'], ':', doc['document'])