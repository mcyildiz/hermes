"""QoR report and run tracking from Python, and the dashboard server."""

import importlib.util
import json
import sqlite3
import threading
import urllib.request
from pathlib import Path

import pytest

import hermes

ROOT = Path(__file__).resolve().parents[1]


def small_design(n=400):
    d = hermes.Design("pyqor")
    d.die = hermes.Rect(0, 0, 60000, 60000)
    d.row_height = 1000
    cells = [d.add_cell(f"u{i}", 2000, 1000) for i in range(n)]
    for i, c in enumerate(cells):
        d.move_cell(c, hermes.Point((i * 7919) % 58000, ((i * 104729) % 59) * 1000))
    for i in range(n - 1):
        net = d.add_net(f"n{i}")
        d.connect(cells[i], net, "z", hermes.Point(1000, 500), hermes.PinDir.Output)
        d.connect(cells[(i + 3) % n], net, "a", hermes.Point(0, 500))
    return d


def test_compute_qor_and_maps():
    d = small_design()
    q = hermes.compute_qor(d)
    assert q.density.avg_util > 0
    assert q.wirelength.hpwl > 0
    assert q.congestion.layer_source.startswith("default")
    assert len(q.congestion.layers) == 6
    assert "density.util" in q.map_names()
    m = q.map("congestion.h.util")
    assert m.nx == 64 and m.ny == 64
    assert len(m.values()) == 64 * 64
    np = pytest.importorskip("numpy")
    a = np.asarray(m)
    assert a.shape == (64, 64) and a.dtype == np.float32
    assert abs(float(a[3, 5]) - m.at(5, 3)) < 1e-6
    j = json.loads(q.to_json())
    assert j["wirelength"]["hpwl"] == q.wirelength.hpwl
    assert "Placement density" in str(q)


def test_routing_layers_property():
    d = small_design(10)
    L = hermes.LayerDir
    d.routing_layers = [hermes.RoutingLayer("m1", L.Horizontal, 200), hermes.RoutingLayer("m2", L.Vertical, 200),
                        hermes.RoutingLayer("m3", L.Horizontal, 400)]
    assert [l.name for l in d.routing_layers] == ["m1", "m2", "m3"]
    opts = hermes.GlobalRouterOptions()
    opts.grid_x = opts.grid_y = 16
    q = hermes.compute_qor(d, hermes.QorOptions(), opts)
    assert q.congestion.layer_source == "lef"
    assert q.congestion.routed


def test_tracked_flow(tmp_path):
    db = tmp_path / "runs.db"
    d = small_design()
    o = hermes.FlowOptions()
    o.placer.max_iterations = 40
    with hermes.track.Run(name="py", design=d.name, tags=["t"], settings_json=o.to_json(), db_path=str(db)) as run:
        assert run.db_ok
        rep = hermes.run_flow(d, o, tracker=run)
        run.metric("custom", 1, 2.5)
        rid = run.id
    assert rep.qor is not None
    c = sqlite3.connect(db)
    status, summary, results = c.execute("SELECT status, summary, results FROM runs WHERE id=?", (rid,)).fetchone()
    assert status == "done"
    assert json.loads(summary)["hpwl"] == pytest.approx(rep.qor.wirelength.hpwl)
    assert json.loads(results)["design"] == "pyqor"
    names = {r[0] for r in c.execute("SELECT DISTINCT name FROM metrics WHERE run_id=?", (rid,))}
    assert {"gp.hpwl", "gp.overflow", "custom"} <= names
    stages = [r[0] for r in c.execute("SELECT name FROM stages WHERE run_id=? ORDER BY seq", (rid,))]
    assert stages[0] == "placement" and stages[-1] == "report"


def test_failed_run_is_recorded(tmp_path):
    db = tmp_path / "runs.db"
    with pytest.raises(RuntimeError):
        with hermes.track.Run(name="boom", db_path=str(db)):
            raise RuntimeError("boom")
    c = sqlite3.connect(db)
    assert c.execute("SELECT status, error FROM runs").fetchone() == ("failed", "boom")


def load_dashboard():
    spec = importlib.util.spec_from_file_location("hermes_dashboard", ROOT / "tools" / "dashboard" / "hermes_dashboard.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_dashboard_push_and_api(tmp_path):
    dash = load_dashboard()
    httpd = dash.serve(str(tmp_path / "central.db"), "127.0.0.1", 0, "tok")
    port = httpd.server_address[1]
    th = threading.Thread(target=httpd.serve_forever, daemon=True)
    th.start()
    try:
        base = f"http://127.0.0.1:{port}"
        d = small_design()
        o = hermes.FlowOptions()
        o.placer.max_iterations = 30
        with hermes.track.Run(name="pushed", design=d.name, db_path="-", server_url=base, token="tok") as run:
            hermes.run_flow(d, o, tracker=run)
            rid = run.id

        def get(path, token="tok"):
            req = urllib.request.Request(base + path, headers={"X-Hermes-Token": token} if token else {})
            return json.load(urllib.request.urlopen(req))

        runs = get("/api/runs")
        assert [r["id"] for r in runs] == [rid]
        assert runs[0]["status"] == "done" and runs[0]["summary"]["hpwl"] > 0
        r = get(f"/api/runs/{rid}")
        assert r["results"]["design"] == "pyqor" and r["has_maps"]
        assert [s["name"] for s in r["stages"]][0] == "placement"
        m = get(f"/api/runs/{rid}/metrics?names=gp.hpwl")
        assert len(m["gp.hpwl"]) > 0
        maps = get(f"/api/runs/{rid}/maps")
        assert any(x["name"] == "density.util" for x in maps)
        ev = get(f"/api/runs/{rid}/events?kind=stage")
        assert ev and ev[0]["kind"] == "stage"
        html = urllib.request.urlopen(urllib.request.Request(base + "/?token=tok")).read()
        assert b"hermes runs" in html
        with pytest.raises(urllib.error.HTTPError):
            get("/api/runs", token="")
    finally:
        httpd.shutdown()
