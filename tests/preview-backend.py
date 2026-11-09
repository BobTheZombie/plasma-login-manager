#!/usr/bin/env python3
"""CI-only fake private socket, used solely for a visual screenshot.

Never install this script with plmd or use it as an authentication service.
"""
import json
import os
import socket
import time

path = "/run/plm/control.sock"
os.makedirs(os.path.dirname(path), exist_ok=True)
try:
    os.unlink(path)
except FileNotFoundError:
    pass

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
    server.bind(path)
    os.chmod(path, 0o600)
    server.listen(2)
    server.settimeout(20)
    with server.accept()[0] as conn:
        users = [
            {"name": "demo", "displayName": "Demo User"},
            {"name": "guest-preview", "displayName": "Second Account"},
        ]
        conn.sendall((json.dumps({
            "type": "hello", "seat": "seat0",
            "session": "Plasma (Wayland)", "users": users,
        }) + "\n").encode())
        conn.settimeout(12)
        try:
            while conn.recv(4096):
                pass
        except socket.timeout:
            pass
