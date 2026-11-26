#!/usr/bin/env python3
"""hermes run dashboard: live and past runs, run details, comparisons.

Standard library only (Python >= 3.9). Reads the run database the engine
writes (default ~/.hermes/runs.db, or $HERMES_RUNS_DB) and also accepts runs
pushed by engines on other machines (`hermes ... --track-server URL`).

    python3 tools/dashboard/hermes_dashboard.py                  # http://localhost:8765
    python3 tools/dashboard/hermes_dashboard.py --host 0.0.0.0 --token SECRET
    # on another machine:
    hermes design_dir --track-server http://<this-host>:8765 --track-token SECRET

With --token, every request needs the token: open http://host:8765/?token=SECRET
once in the browser (it is kept in a cookie), engines send it as a header.
For access from outside your network, use an SSH tunnel
(ssh -L 8765:localhost:8765 mac-mini) or a VPN such as Tailscale; the server
speaks plain HTTP.
"""

from __future__ import annotations

import argparse
import hmac
import json
import os
import sqlite3
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
LOST_AFTER_S = 15.0  # a running run without a heartbeat for this long is shown as "lost"

SCHEMA = """
CREATE TABLE IF NOT EXISTS runs(
 id TEXT PRIMARY KEY, name TEXT, status TEXT, started REAL, ended REAL, heartbeat REAL,
 design TEXT, input TEXT, command TEXT, cwd TEXT, host TEXT, os TEXT, cpu TEXT, cores INTEGER,
 mem_bytes INTEGER, threads INTEGER, version TEXT, pid INTEGER, user TEXT, tags TEXT, settings TEXT,
 progress REAL, stage TEXT, message TEXT, summary TEXT, results TEXT, maps TEXT,
 exit_code INTEGER, error TEXT, peak_rss INTEGER, dropped INTEGER, origin TEXT);
CREATE TABLE IF NOT EXISTS stages(run_id TEXT, seq INTEGER, name TEXT, started REAL, ended REAL,
 PRIMARY KEY(run_id, seq));
CREATE TABLE IF NOT EXISTS events(run_id TEXT, seq INTEGER, t REAL, kind TEXT, level TEXT,
 channel TEXT, msg_id TEXT, text TEXT, PRIMARY KEY(run_id, seq));
CREATE TABLE IF NOT EXISTS metrics(run_id TEXT, name TEXT, step INTEGER, t REAL, value REAL);
CREATE INDEX IF NOT EXISTS metrics_run ON metrics(run_id, name, step);
CREATE INDEX IF NOT EXISTS runs_started ON runs(started);
CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT);
INSERT OR IGNORE INTO meta VALUES('schema_version', '1');
"""

RUN_COLUMNS = [
    "name", "status", "started", "ended", "heartbeat", "design", "input", "command", "cwd", "host", "os",
    "cpu", "cores", "mem_bytes", "threads", "version", "pid", "user", "tags", "settings", "progress",
    "stage", "message", "summary", "results", "maps", "exit_code", "error", "peak_rss", "dropped",
]
LIST_COLUMNS = [
    "id", "name", "status", "started", "ended", "heartbeat", "design", "input", "host", "cores", "threads",
    "version", "tags", "progress", "stage", "message", "summary", "exit_code", "error", "peak_rss", "origin",
]


def default_db() -> str:
    return os.environ.get("HERMES_RUNS_DB") or str(Path.home() / ".hermes" / "runs.db")


class Store:
    """SQLite access; one connection per thread (WAL: readers never block the engine)."""

    def __init__(self, path: str):
        self.path = path
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        self.local = threading.local()
        self.write_lock = threading.Lock()
        with self.conn() as c:
            c.execute("PRAGMA journal_mode=WAL")
            c.executescript(SCHEMA)

    def conn(self) -> sqlite3.Connection:
        c = getattr(self.local, "c", None)
        if c is None:
            c = sqlite3.connect(self.path, timeout=10)
            c.row_factory = sqlite3.Row
            self.local.c = c
        return c

    # --- queries ---------------------------------------------------------------
    @staticmethod
    def _status(row: dict) -> str:
        s = row.get("status") or "running"
        if s == "running" and row.get("heartbeat") and time.time() - row["heartbeat"] > LOST_AFTER_S:
            return "lost"
        return s

    @staticmethod
    def _json(v, default):
        if not v:
            return default
        try:
            return json.loads(v)
        except ValueError:
            return default

    def runs(self, limit: int = 200, q: str = "") -> list[dict]:
        sql = f"SELECT {','.join(LIST_COLUMNS)} FROM runs"
        args: list = []
        if q:
            sql += " WHERE id LIKE ? OR name LIKE ? OR design LIKE ? OR host LIKE ? OR tags LIKE ? OR input LIKE ?"
            args = [f"%{q}%"] * 6
        sql += " ORDER BY started DESC LIMIT ?"
        args.append(limit)
        out = []
        for r in self.conn().execute(sql, args):
            d = dict(r)
            d["status"] = self._status(d)
            d["summary"] = self._json(d.get("summary"), {})
            d["tags"] = self._json(d.get("tags"), [])
            out.append(d)
        return out

    def run(self, run_id: str) -> dict | None:
        r = self.conn().execute("SELECT * FROM runs WHERE id=?", (run_id,)).fetchone()
        if not r:
            return None
        d = dict(r)
        d.pop("maps", None)
        d["status"] = self._status(d)
        for k, default in (("summary", {}), ("results", None), ("settings", {}), ("tags", [])):
            d[k] = self._json(d.get(k), default)
        d["stages"] = [dict(s) for s in self.conn().execute(
            "SELECT seq, name, started, ended FROM stages WHERE run_id=? ORDER BY seq", (run_id,))]
        d["metric_names"] = [x[0] for x in self.conn().execute(
            "SELECT DISTINCT name FROM metrics WHERE run_id=?", (run_id,))]
        d["has_maps"] = bool(r["maps"])
        d["now"] = time.time()
        return d

    def events(self, run_id: str, after: int = 0, limit: int = 1000, kinds: str = "") -> list[dict]:
        sql = "SELECT seq, t, kind, level, channel, msg_id, text FROM events WHERE run_id=? AND seq>?"
        args: list = [run_id, after]
        if kinds:
            ks = kinds.split(",")
            sql += f" AND kind IN ({','.join('?' * len(ks))})"
            args += ks
        sql += " ORDER BY seq LIMIT ?"
        args.append(limit)
        return [dict(r) for r in self.conn().execute(sql, args)]

    def metrics(self, run_id: str, names: list[str], max_points: int = 2000) -> dict:
        out = {}
        for n in names:
            rows = self.conn().execute(
                "SELECT step, value, t FROM metrics WHERE run_id=? AND name=? ORDER BY step, t", (run_id, n)).fetchall()
            if len(rows) > max_points:
                stride = len(rows) / max_points
                rows = [rows[int(i * stride)] for i in range(max_points)] + [rows[-1]]
            out[n] = [[r[0], r[1], r[2]] for r in rows]
        return out

    def maps(self, run_id: str):
        r = self.conn().execute("SELECT maps FROM runs WHERE id=?", (run_id,)).fetchone()
        return self._json(r[0] if r else None, [])

    # --- ingest (runs pushed by engines on other machines) ---------------------
    def ingest(self, batch: dict, origin: str) -> int:
        run = batch.get("run") or {}
        rid = run.get("id")
        if not rid or not isinstance(rid, str):
            raise ValueError("batch without run id")
        with self.write_lock:
            c = self.conn()
            c.execute("INSERT OR IGNORE INTO runs(id, origin) VALUES(?, ?)", (rid, origin))
            cols = [k for k in RUN_COLUMNS if k in run]
            if cols:
                vals = [run[k] if not isinstance(run[k], (dict, list)) else json.dumps(run[k]) for k in cols]
                c.execute(f"UPDATE runs SET {','.join(k + '=?' for k in cols)}, origin=? WHERE id=?",
                          vals + [origin, rid])
            c.executemany("INSERT OR REPLACE INTO events VALUES(?,?,?,?,?,?,?,?)",
                          [(rid, e.get("seq"), e.get("t"), e.get("kind"), e.get("level"), e.get("channel"),
                            e.get("msg_id"), e.get("text")) for e in batch.get("events", [])])
            c.executemany("INSERT INTO metrics VALUES(?,?,?,?,?)",
                          [(rid, m.get("name"), m.get("step"), m.get("t"), m.get("value"))
                           for m in batch.get("metrics", [])])
            c.executemany("INSERT OR REPLACE INTO stages VALUES(?,?,?,?,?)",
                          [(rid, s.get("seq"), s.get("name"), s.get("started"), s.get("ended"))
                           for s in batch.get("stages", [])])
            c.commit()
        return len(batch.get("events", [])) + len(batch.get("metrics", []))


def make_handler(store: Store, token: str | None):
    index_html = (HERE / "index.html").read_bytes()

    class Handler(BaseHTTPRequestHandler):
        server_version = "hermes-dashboard/1"

        def log_message(self, fmt, *args):  # quiet; errors still go to stderr
            pass

        def _send(self, code: int, body: bytes, ctype: str = "application/json", extra: dict | None = None):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(body)

        def _json(self, obj, code: int = 200):
            self._send(code, json.dumps(obj, separators=(",", ":")).encode())

        def _authorized(self, query: dict) -> tuple[bool, dict]:
            if not token:
                return True, {}
            given = self.headers.get("X-Hermes-Token", "")
            if not given:
                for part in self.headers.get("Cookie", "").split(";"):
                    k, _, v = part.strip().partition("=")
                    if k == "hermes_token":
                        given = urllib.parse.unquote(v)
            q = query.get("token", [""])[0]
            if q and hmac.compare_digest(q, token):
                return True, {"Set-Cookie": f"hermes_token={urllib.parse.quote(token)}; Path=/; HttpOnly; SameSite=Strict"}
            return bool(given) and hmac.compare_digest(given, token), {}

        def do_GET(self):  # noqa: N802
            u = urllib.parse.urlparse(self.path)
            qs = urllib.parse.parse_qs(u.query)
            ok, extra = self._authorized(qs)
            if not ok:
                return self._send(401, b"token required: open /?token=...", "text/plain")
            parts = [p for p in u.path.split("/") if p]
            try:
                if not parts or parts == ["index.html"]:
                    return self._send(200, index_html, "text/html; charset=utf-8", extra)
                if parts[0] != "api":
                    return self._send(404, b"not found", "text/plain")
                if parts[1:] == ["runs"]:
                    return self._json(store.runs(int(qs.get("limit", ["200"])[0]), qs.get("q", [""])[0]))
                if len(parts) >= 3 and parts[1] == "runs":
                    rid = parts[2]
                    if len(parts) == 3:
                        r = store.run(rid)
                        return self._json(r) if r else self._json({"error": "no such run"}, 404)
                    if parts[3] == "events":
                        return self._json(store.events(rid, int(qs.get("after", ["0"])[0]),
                                                       min(5000, int(qs.get("limit", ["1000"])[0])),
                                                       qs.get("kind", [""])[0]))
                    if parts[3] == "metrics":
                        names = [n for n in qs.get("names", [""])[0].split(",") if n]
                        return self._json(store.metrics(rid, names))
                    if parts[3] == "maps":
                        return self._json(store.maps(rid))
                if parts[1:] == ["health"]:
                    return self._json({"ok": True, "db": store.path, "time": time.time()})
                return self._json({"error": "not found"}, 404)
            except (ValueError, IndexError) as e:
                return self._json({"error": str(e)}, 400)

        def do_POST(self):  # noqa: N802
            u = urllib.parse.urlparse(self.path)
            ok, _ = self._authorized({})
            if not ok:
                return self._json({"error": "bad token"}, 401)
            if u.path.rstrip("/") != "/api/ingest":
                return self._json({"error": "not found"}, 404)
            n = int(self.headers.get("Content-Length", "0"))
            if n <= 0 or n > 256 * 1024 * 1024:
                return self._json({"error": "bad length"}, 400)
            try:
                batch = json.loads(self.rfile.read(n))
                k = store.ingest(batch, f"push:{self.client_address[0]}")
            except (ValueError, sqlite3.Error) as e:
                return self._json({"error": str(e)}, 400)
            return self._json({"ok": True, "rows": k})

    return Handler


def serve(db: str, host: str, port: int, token: str | None) -> ThreadingHTTPServer:
    store = Store(db)
    httpd = ThreadingHTTPServer((host, port), make_handler(store, token))
    httpd.daemon_threads = True
    return httpd


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--db", default=default_db(), help="run database (default: %(default)s)")
    ap.add_argument("--host", default="127.0.0.1", help="bind address; 0.0.0.0 = reachable from other machines")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--token", default=os.environ.get("HERMES_DASHBOARD_TOKEN"),
                    help="require this token for every request (env HERMES_DASHBOARD_TOKEN)")
    a = ap.parse_args(argv)
    if a.host not in ("127.0.0.1", "localhost", "::1") and not a.token:
        print("warning: listening on a network address without --token; anyone who can reach it can see runs",
              file=sys.stderr)
    httpd = serve(a.db, a.host, a.port, a.token)
    shown = "localhost" if a.host in ("0.0.0.0", "::") else a.host
    print(f"hermes dashboard: http://{shown}:{httpd.server_address[1]}/  (database {a.db})", flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
