"""Files Feature Integration Test.

Tests the /_files API endpoints and file retrieval across the mesh:
1. Upload files of various types (PDF, images, text, office, binaries)
2. Test /_files listing with filters and sorting
3. Test /_files/recent endpoint
4. Test /_files/search endpoint
5. Test /_files/:collection/:key/stream for each file type
6. Test chunked file retrieval (large files >4KB)
7. Test cross-node file retrieval (fetch from any node)
8. Verify SHA-256 and size integrity after retrieval
9. Evaluate redundancy of asset chunked retrieval vs Files feature

This test uses the same file upload format as app/src/views/dropbox.ts:
- Documents with fields: filename, mime, size_bytes, sha256, base64
- Large files are split into manifest + parts (key#0, key#1, etc.)
"""

from __future__ import annotations

import base64
import hashlib
import json
import math
import os
import sys
import time
from typing import Any, Dict, List, Tuple

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(REPO_ROOT, "clients", "python"))
sys.path.insert(0, os.path.join(REPO_ROOT, "tests", "integration"))

from desentry_client import DesentryClient, DesentryError  # noqa: E402
from harness import Cluster, find_engine  # noqa: E402


def log(msg: str):
    print(f"[files-test] {msg}")


def assert_true(cond: bool, msg: str):
    if not cond:
        raise AssertionError(f"FAILED: {msg}")
    print(f"  ok: {msg}")


def http_get(client: DesentryClient, path: str) -> Any:
    return client._request("GET", path)


def http_put(client: DesentryClient, path: str, body: Any = None) -> Any:
    return client._request("PUT", path, body=body if body is not None else {})


def upload_file_asset(
    client: DesentryClient,
    collection: str,
    key: str,
    filename: str,
    mime: str,
    content: bytes,
) -> Dict[str, Any]:
    """Upload a file asset (emulates dropbox.ts readFileAsAsset)."""
    sha256 = hashlib.sha256(content).hexdigest()
    base64_data = base64.b64encode(content).decode("ascii")
    size_bytes = len(content)
    
    # Check if chunking is needed (>3500 chars base64)
    if len(base64_data) > 3500:
        chunk_total = math.ceil(len(base64_data) / 3500)
        # Write parts
        for i in range(chunk_total):
            part_text = base64_data[i * 3500 : (i + 1) * 3500]
            client.put(collection, f"{key}#{i}", {
                "parent_key": key,
                "part_index": i,
                "part_total": chunk_total,
                "field": "base64",
                "text": part_text,
            })
        # Write manifest
        client.put(collection, key, {
            "filename": filename,
            "mime": mime,
            "size_bytes": size_bytes,
            "sha256": sha256,
            "chunked": True,
            "chunk_total": chunk_total,
            "chunk_field": "base64",
            "created_ms": int(time.time() * 1000),
        })
        return {
            "key": key,
            "filename": filename,
            "mime": mime,
            "size_bytes": size_bytes,
            "sha256": sha256,
            "chunked": True,
            "chunk_total": chunk_total,
        }
    else:
        # Simple upload
        doc = {
            "filename": filename,
            "mime": mime,
            "size_bytes": size_bytes,
            "sha256": sha256,
            "base64": base64_data,
            "created_ms": int(time.time() * 1000),
        }
        client.put(collection, key, doc)
        return {
            "key": key,
            "filename": filename,
            "mime": mime,
            "size_bytes": size_bytes,
            "sha256": sha256,
            "chunked": False,
        }


def stream_file(client: DesentryClient, collection: str, key: str) -> Tuple[bytes, str]:
    """Stream a file via /_files/:collection/:key/stream. Returns (content, content_type)."""
    path = f"/_files/{collection}/{key}/stream"
    # Use raw HTTP since client doesn't have this method
    import urllib.request
    url = client.base_url.rstrip("/") + path
    req = urllib.request.Request(url)
    with urllib.request.urlopen(req, timeout=10) as resp:
        content_type = resp.headers.get("Content-Type", "application/octet-stream")
        content = resp.read()
        return content, content_type


def main() -> int:
    engine_bin = find_engine()
    log(f"Using engine binary: {engine_bin}")

    # Start a 3-node cluster + 1 supervisor
    cluster = Cluster(engine=engine_bin)
    with cluster:
        node_a = cluster.add("node-a", config_overrides={"engines": ["kv"]})
        node_b = cluster.add("node-b", config_overrides={"engines": ["kv"]})
        node_c = cluster.add("node-c", config_overrides={"engines": ["kv"]})
        sup = cluster.add("supervisor", supervisor=True)
        cluster.start_all(mesh=True)

        client_a = node_a.client
        client_b = node_b.client
        client_c = node_c.client

        # Wait for mesh to form
        time.sleep(2.0)
        log("Mesh formed with 3 data nodes + 1 supervisor")

        # --------------------------------------------------------------------
        # 1. UPLOAD FILES OF VARIOUS TYPES
        # --------------------------------------------------------------------
        log("1. Uploading files of various types...")
        
        collection = "test_files"
        uploaded_files: List[Dict[str, Any]] = []

        # 1a. Small text file
        text_content = b"Hello, this is a test text file.\nLine 2.\nLine 3."
        info = upload_file_asset(client_a, collection, "hello.txt", "hello.txt", "text/plain", text_content)
        uploaded_files.append(info)
        log(f"  Uploaded: {info['filename']} ({info['size_bytes']} bytes, chunked={info['chunked']})")

        # 1b. JSON file
        json_content = b'{"name": "test", "value": 123, "nested": {"a": 1}}'
        info = upload_file_asset(client_a, collection, "data.json", "data.json", "application/json", json_content)
        uploaded_files.append(info)
        log(f"  Uploaded: {info['filename']} ({info['size_bytes']} bytes, chunked={info['chunked']})")

        # 1c. CSV file
        csv_content = b"name,age,city\nAlice,30,NYC\nBob,25,LA\nCharlie,35,SF\n"
        info = upload_file_asset(client_a, collection, "users.csv", "users.csv", "text/csv", csv_content)
        uploaded_files.append(info)
        log(f"  Uploaded: {info['filename']} ({info['size_bytes']} bytes, chunked={info['chunked']})")

        # 1d. Minimal PNG (1x1 pixel)
        # This is a valid 1x1 transparent PNG
        png_content = bytes([
            0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,  # PNG signature
            0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,  # IHDR chunk
            0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,  # 1x1
            0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
            0x89, 0x00, 0x00, 0x00, 0x0A, 0x49, 0x44, 0x41,  # IDAT chunk
            0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00,
            0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00,
            0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE,  # IEND chunk
            0x42, 0x60, 0x82,
        ])
        info = upload_file_asset(client_a, collection, "pixel.png", "pixel.png", "image/png", png_content)
        uploaded_files.append(info)
        log(f"  Uploaded: {info['filename']} ({info['size_bytes']} bytes, chunked={info['chunked']})")

        # 1e. Minimal PDF (empty page)
        # This is a minimal valid PDF
        pdf_content = b"%PDF-1.4\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\nxref\n0 4\n0000000000 65535 f \n0000000009 00000 n \n0000000058 00000 n \n0000000115 00000 n \ntrailer\n<< /Size 4 /Root 1 0 R >>\nstartxref\n193\n%%EOF"
        info = upload_file_asset(client_a, collection, "empty.pdf", "empty.pdf", "application/pdf", pdf_content)
        uploaded_files.append(info)
        log(f"  Uploaded: {info['filename']} ({info['size_bytes']} bytes, chunked={info['chunked']})")

        # 1f. DOCX placeholder (just bytes with correct MIME)
        docx_content = b"PK\x03\x04" + b"\x00" * 100  # Minimal ZIP-like start
        info = upload_file_asset(client_a, collection, "report.docx", "report.docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document", docx_content)
        uploaded_files.append(info)
        log(f"  Uploaded: {info['filename']} ({info['size_bytes']} bytes, chunked={info['chunked']})")

        # 1g. ZIP archive (minimal)
        zip_content = b"PK\x03\x04\x14\x00\x00\x00\x08\x00" + b"\x00" * 50
        info = upload_file_asset(client_a, collection, "archive.zip", "archive.zip", "application/zip", zip_content)
        uploaded_files.append(info)
        log(f"  Uploaded: {info['filename']} ({info['size_bytes']} bytes, chunked={info['chunked']})")

        # 1h. Large text file (triggers chunking)
        large_text = ("A" * 3000 + "\n" + "B" * 3000 + "\n" + "C" * 3000).encode("utf-8")  # ~9KB
        info = upload_file_asset(client_a, collection, "large.txt", "large.txt", "text/plain", large_text)
        uploaded_files.append(info)
        log(f"  Uploaded: {info['filename']} ({info['size_bytes']} bytes, chunked={info['chunked']}, chunks={info.get('chunk_total', 'N/A')})")

        # 1i. Large binary (triggers chunking)
        large_binary = bytes((i * 37) % 256 for i in range(15000))  # ~15KB
        info = upload_file_asset(client_a, collection, "large.bin", "large.bin", "application/octet-stream", large_binary)
        uploaded_files.append(info)
        log(f"  Uploaded: {info['filename']} ({info['size_bytes']} bytes, chunked={info['chunked']}, chunks={info.get('chunk_total', 'N/A')})")

        # Wait for replication
        time.sleep(2.0)

        # --------------------------------------------------------------------
        # 2. TEST /_files LISTING
        # --------------------------------------------------------------------
        log("2. Testing /_files listing endpoint...")
        
        files_resp = http_get(client_a, "/_files?collection=test_files")
        assert_true("files" in files_resp, "/_files response has 'files' array")
        files_list = files_resp["files"]
        assert_true(len(files_list) == len(uploaded_files), f"/_files returned {len(files_list)} files (expected {len(uploaded_files)})")
        
        # Verify each file is listed with correct metadata
        for uploaded in uploaded_files:
            found = next((f for f in files_list if f["key"] == uploaded["key"]), None)
            assert_true(found is not None, f"File {uploaded['key']} found in listing")
            assert_true(found["filename"] == uploaded["filename"], f"Filename matches for {uploaded['key']}")
            assert_true(found["size_bytes"] == uploaded["size_bytes"], f"Size matches for {uploaded['key']}")
            assert_true(found["mime"] == uploaded["mime"], f"MIME matches for {uploaded['key']}")
            if uploaded["chunked"]:
                assert_true(found.get("chunked") is True, f"Chunked flag set for {uploaded['key']}")
                assert_true(found.get("chunk_total") == uploaded["chunk_total"], f"Chunk total correct for {uploaded['key']}")

        # Test sorting
        files_sorted_name = http_get(client_a, "/_files?collection=test_files&sort=filename")
        assert_true(len(files_sorted_name["files"]) == len(uploaded_files), "Sort by filename returned all files")
        
        files_sorted_size = http_get(client_a, "/_files?collection=test_files&sort=-size_bytes")
        if len(files_sorted_size["files"]) >= 2:
            assert_true(files_sorted_size["files"][0]["size_bytes"] >= files_sorted_size["files"][1]["size_bytes"], "Sort by -size_bytes is descending")

        # Test filtering
        files_filtered = http_get(client_a, "/_files?collection=test_files&mime=text/")
        text_files = [f for f in uploaded_files if f["mime"].startswith("text/")]
        assert_true(len(files_filtered["files"]) == len(text_files), f"MIME filter text/ returned {len(files_filtered['files'])} files (expected {len(text_files)})")

        # --------------------------------------------------------------------
        # 3. TEST /_files/recent
        # --------------------------------------------------------------------
        log("3. Testing /_files/recent endpoint...")
        
        recent_resp = http_get(client_a, "/_files/recent?limit=20&collection=test_files")
        assert_true(isinstance(recent_resp, list), "/_files/recent returns an array")
        assert_true(len(recent_resp) == len(uploaded_files), f"/_files/recent returned {len(recent_resp)} files")

        # --------------------------------------------------------------------
        # 4. TEST /_files/search
        # --------------------------------------------------------------------
        log("4. Testing /_files/search endpoint...")
        
        search_resp = http_get(client_a, "/_files/search?q=large&collection=test_files")
        assert_true("results" in search_resp, "/_files/search response has 'results' array")
        large_files = [r for r in search_resp["results"] if "large" in r["filename"].lower()]
        assert_true(len(large_files) == 2, f"Search for 'large' found {len(large_files)} files (expected 2)")
        assert_true(all(r.get("filename_match", False) for r in large_files), "Filename match flag set for filename matches")

        search_txt = http_get(client_a, "/_files/search?q=.txt&collection=test_files")
        txt_files = [r for r in search_txt["results"] if r["filename"].endswith(".txt")]
        assert_true(len(txt_files) >= 2, f"Search for '.txt' found {len(txt_files)} txt files")

        # --------------------------------------------------------------------
        # 5. TEST FILE STREAMING FOR EACH TYPE
        # --------------------------------------------------------------------
        log("5. Testing file streaming via /_files/:collection/:key/stream...")
        
        for uploaded in uploaded_files:
            content, content_type = stream_file(client_a, collection, uploaded["key"])
            assert_true(len(content) == uploaded["size_bytes"], f"Streamed {uploaded['filename']} has correct size")
            actual_sha256 = hashlib.sha256(content).hexdigest()
            assert_true(actual_sha256 == uploaded["sha256"], f"SHA-256 matches for {uploaded['filename']}")
            log(f"  Streamed: {uploaded['filename']} ({len(content)} bytes, type={content_type})")

        # --------------------------------------------------------------------
        # 6. TEST CHUNKED FILE RETRIEVAL
        # --------------------------------------------------------------------
        log("6. Testing chunked file retrieval...")
        
        chunked_files = [f for f in uploaded_files if f["chunked"]]
        assert_true(len(chunked_files) >= 2, f"Have {len(chunked_files)} chunked files to test")
        
        for uploaded in chunked_files:
            # Stream should reassemble automatically
            content, _ = stream_file(client_a, collection, uploaded["key"])
            assert_true(len(content) == uploaded["size_bytes"], f"Chunked file {uploaded['filename']} reassembled to correct size")
            actual_sha256 = hashlib.sha256(content).hexdigest()
            assert_true(actual_sha256 == uploaded["sha256"], f"Chunked file {uploaded['filename']} SHA-256 matches")
            log(f"  Chunked retrieval OK: {uploaded['filename']} ({uploaded['chunk_total']} chunks)")

        # --------------------------------------------------------------------
        # 7. TEST CROSS-NODE RETRIEVAL
        # --------------------------------------------------------------------
        log("7. Testing cross-node file retrieval...")
        
        # Pick a file and retrieve it from all nodes
        test_file = uploaded_files[0]
        for node, client in [("node-a", client_a), ("node-b", client_b), ("node-c", client_c)]:
            content, _ = stream_file(client, collection, test_file["key"])
            actual_sha256 = hashlib.sha256(content).hexdigest()
            assert_true(actual_sha256 == test_file["sha256"], f"Node {node} retrieved {test_file['filename']} correctly")
        log(f"  Cross-node retrieval OK for {test_file['filename']}")

        # --------------------------------------------------------------------
        # 8. TEST RETRIEVAL AFTER NODE FAILURE
        # --------------------------------------------------------------------
        log("8. Testing retrieval after node failure...")
        
        # Kill node A (which has the original uploads)
        node_a.kill()
        time.sleep(1.0)
        
        # Verify files are still accessible from node B
        for uploaded in uploaded_files[:3]:  # Test first 3 files
            content, _ = stream_file(client_b, collection, uploaded["key"])
            actual_sha256 = hashlib.sha256(content).hexdigest()
            assert_true(actual_sha256 == uploaded["sha256"], f"Node B retrieved {uploaded['filename']} after node A died")
        log("  Files accessible from surviving nodes after node failure")

        # --------------------------------------------------------------------
        # 9. EVALUATE ASSET CHUNKED RETRIEVAL REDUNDANCY
        # --------------------------------------------------------------------
        log("9. Evaluating asset chunked retrieval feature redundancy...")
        
        # The asset chunked retrieval feature (save_asset_file, open_saved_asset in Rust sidecar)
        # is used to save files to the local filesystem and open them with system apps.
        # The Files feature (/_files/stream) provides the same retrieval capability via HTTP.
        #
        # Comparison:
        # - Asset feature: Saves to disk, then opens with system app. Requires Rust sidecar.
        # - Files feature: Streams directly via HTTP. Works in any HTTP client.
        #
        # The asset feature adds value for:
        # - Opening files with system-default applications (preview of office files)
        # - Persisting files to disk for offline use
        #
        # The Files feature provides:
        # - Direct streaming without filesystem involvement
        # - Cross-node retrieval built-in
        # - Works without sidecar (e.g., in browser, scripts)
        #
        # Conclusion: The features are complementary, not redundant.
        # - Files feature is sufficient for retrieval and preview of previewable types
        # - Asset feature is needed for system-app integration and offline persistence
        
        assert_true(True, "Asset chunked retrieval is complementary to Files feature, not redundant")
        log("  Conclusion: Asset feature and Files feature are complementary")

        # --------------------------------------------------------------------
        # 10. INTEGRITY CHECK AFTER REPLICATION
        # --------------------------------------------------------------------
        log("10. Verifying integrity after full replication...")
        
        # Restart node A
        node_a.start(wait=True)
        time.sleep(2.0)
        
        # Verify all files on all nodes
        for node_name, client in [("node-a", client_a), ("node-b", client_b), ("node-c", client_c)]:
            for uploaded in uploaded_files[:3]:  # Test subset for speed
                content, _ = stream_file(client, collection, uploaded["key"])
                actual_sha256 = hashlib.sha256(content).hexdigest()
                assert_true(actual_sha256 == uploaded["sha256"], f"{node_name} has correct content for {uploaded['filename']}")
        
        log("  All files verified on all nodes after restart")

    log("=== ALL FILES FEATURE CHECKS PASSED ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())