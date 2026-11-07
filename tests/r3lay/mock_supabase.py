#!/usr/bin/env python3
"""A pretend Supabase for testing apps/r3lay with no network and no real data.

    python3 tests/r3lay/mock_supabase.py PORT LOGFILE

Knows the endpoints the app uses and just enough of PostgREST's filters. Every
write is logged to LOGFILE as a line of JSON. Whoever signs in with the
password "good" is alice. A message to bob gets a reply on the next read.
"""
import json
import os
import re
import struct
import sys
import time
import urllib.parse
import uuid
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ALICE = "aaaaaaaa-0000-4000-8000-000000000001"
BOB = "bbbbbbbb-0000-4000-8000-000000000002"
CARA = "cccccccc-0000-4000-8000-000000000003"
GROUP = "11111111-0000-4000-8000-00000000000a"
KEY = "test-public-key"
TOKEN = "access-1"

profiles = [
    {"id": ALICE, "username": "alice", "bio": "Caf\u00e9 lover", "avatar_url": None},
    {"id": BOB, "username": "bob", "avatar_url": "http://127.0.0.1:%s/avatar.png" % sys.argv[1]},
    {"id": CARA, "username": "Caroline"},
]


def stamp(seconds_ago=0):
    t = time.time() - seconds_ago
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(t)) + ".%06d+00:00" % ((t % 1) * 1e6)


def row(sender, to, text, ago, read=True):
    return {"id": str(uuid.uuid4()), "client_ref": str(uuid.uuid4()), "sender_id": sender, "recipient_id": to,
            "text_content": text, "media_url": None, "media_type": None, "created_at": stamp(ago),
            "read_at": stamp(ago - 1) if read else None, "deleted_at": None}


messages = [
    row(BOB, ALICE, "hey, this is the oldest one", 90000),
    row(ALICE, BOB, "a long one to check the wrapping works: " + "lots of words go here and wrap over the lines " * 3, 500),
    row(BOB, ALICE, "Café crème ☕ à tout à l'heure", 400),
    row(BOB, ALICE, "\U0001f4de Voice call · 3 min 02 s", 300),
    row(BOB, ALICE, "\U0001f600\U0001f600", 200, read=False),
    row(CARA, ALICE, "hi from cara", 3000, read=False),
]
group_messages = [
    {"id": str(uuid.uuid4()), "client_ref": str(uuid.uuid4()), "group_id": GROUP, "sender_id": BOB,
     "text_content": "group hello", "media_type": None, "created_at": stamp(100), "deleted_at": None},
]
group_reads = []
log = None


def png_chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def make_avatar():
    """A small picture: red over blue."""
    rows = b"".join(b"\0" + ((b"\xff\0\0" if y < 16 else b"\0\0\xff") * 32) for y in range(32))
    return b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", struct.pack(">IIBBBBB", 32, 32, 8, 2, 0, 0, 0)) + \
        png_chunk(b"IDAT", zlib.compress(rows)) + png_chunk(b"IEND", b"")


AVATAR = make_avatar()
owed_replies = 0


def record(what, **fields):
    log.write(json.dumps({"what": what, **fields}) + "\n")
    log.flush()


def match(rows, query):
    out = list(rows)
    for key, values in query.items():
        v = values[0]
        if key in ("select", "order", "limit", "or", "on_conflict"):
            continue
        if v.startswith("eq."):
            out = [r for r in out if str(r.get(key)) == v[3:]]
        elif v.startswith("neq."):
            out = [r for r in out if str(r.get(key)) != v[4:]]
        elif v.startswith("gt."):
            out = [r for r in out if r.get(key, "") > v[3:]]
        elif v.startswith("in.("):
            out = [r for r in out if r.get(key) in v[4:-1].split(",")]
        elif v == "is.null":
            out = [r for r in out if r.get(key) is None]
        elif v == "not.is.null":
            out = [r for r in out if r.get(key) is not None]
        elif v.startswith("ilike.*"):
            out = [r for r in out if v[7:-1].lower() in str(r.get(key)).lower()]
    # or=(and(sender_id.eq.A,recipient_id.eq.B),and(...)) and or=(sender_id.eq.A,recipient_id.eq.A)
    if "or" in query:
        ids = set(re.findall(r"[0-9a-f-]{36}", query["or"][0]))
        out = [r for r in out if r.get("sender_id") in ids and r.get("recipient_id") in ids] if len(ids) == 2 \
            else [r for r in out if ids & {r.get("sender_id"), r.get("recipient_id")}]
    order = query.get("order", ["created_at.asc"])[0]
    out.sort(key=lambda r: r.get("created_at", ""), reverse=order.endswith(".desc"))
    return out[:int(query.get("limit", ["1000"])[0])]


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, status, body):
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def authorised(self):
        return self.headers.get("apikey") == KEY and self.headers.get("Authorization") == "Bearer " + TOKEN

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return json.loads(self.rfile.read(n) or b"{}")

    def tokens(self):
        return {"access_token": TOKEN, "refresh_token": "refresh-1", "expires_in": 3600, "user": {"id": ALICE}}

    def rest(self):
        url = urllib.parse.urlsplit(self.path)
        return url.path, urllib.parse.parse_qs(url.query, keep_blank_values=True)

    def do_GET(self):
        global owed_replies
        path, q = self.rest()
        if path == "/avatar.png":               # public, like Supabase storage
            data = AVATAR
            record("avatar")
            self.send_response(200)
            self.send_header("Content-Type", "image/png")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        if not self.authorised():
            return self.reply(401, {"message": "JWT expired"})
        table = path.rsplit("/", 1)[1]
        if table == "messages":
            while owed_replies:
                owed_replies -= 1
                messages.append(row(BOB, ALICE, "got it, thanks", 0, read=False))
            return self.reply(200, match(messages, q))
        if table == "group_messages":
            return self.reply(200, match(group_messages, q))
        if table == "profiles":
            return self.reply(200, match(profiles, q))
        if table == "group_members":
            return self.reply(200, [{"group_id": GROUP, "groups": {"name": "The group"}}])
        if table == "group_reads":
            return self.reply(200, match(group_reads, q))
        self.reply(404, {})

    def do_POST(self):
        global owed_replies
        path, q = self.rest()
        data = self.body()
        if path == "/auth/v1/token":
            grant = q["grant_type"][0]
            record("token", grant=grant)
            good = data.get("password") == "good" if grant == "password" else data.get("refresh_token") == "refresh-1"
            return self.reply(200 if good else 400, self.tokens() if good else {"error": "invalid_grant"})
        if not self.authorised():
            return self.reply(401, {"message": "JWT expired"})
        if path == "/auth/v1/logout":
            record("logout")
            return self.reply(204, {})
        if path == "/rest/v1/rpc/touch_login":
            record("touch_login")
            return self.reply(200, {})
        table = path.rsplit("/", 1)[1]
        if table == "messages":
            r = {**row(data["sender_id"], data["recipient_id"], data["text_content"], 0, read=False), **data}
            record("message", text=r["text_content"], ref=r["client_ref"], to=r["recipient_id"], sender=r["sender_id"])
            messages.append(r)
            owed_replies += 1
            if os.environ.get("MOCK_LOSE_REPLY"):       # it arrived; the answer never does
                self.close_connection = True
                return

            return self.reply(201, [r])
        if table == "group_messages":
            r = {"id": str(uuid.uuid4()), "created_at": stamp(), "deleted_at": None, "media_type": None, **data}
            record("group_message", text=r["text_content"])
            group_messages.append(r)
            return self.reply(201, [r])
        if table == "group_reads":
            record("group_read", group=data["group_id"], prefer=self.headers.get("Prefer"))
            return self.reply(201, [])
        self.reply(404, {})

    def do_PATCH(self):
        path, q = self.rest()
        if not self.authorised():
            return self.reply(401, {"message": "JWT expired"})
        data = self.body()
        if path.endswith("/profiles"):
            for r in match(profiles, q):
                r.update(data)
            record("profile", **data)
            return self.reply(204, {})
        rows = match(messages, q)
        for r in rows:
            r.update(data)
        record("marked_read", count=len(rows))
        self.reply(204, {})


log = open(sys.argv[2], "a")
ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), Handler).serve_forever()
