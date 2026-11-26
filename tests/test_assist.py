"""hermes.Console: the console/tool layer shared with the GUI and the AI assistant."""

import json
import os

import pytest

import hermes

DATA = os.path.join(os.path.dirname(__file__), "data")


def test_console_tools_and_commands():
    c = hermes.Console(DATA)
    assert c.has_design
    s = json.loads(c.summary())
    assert s["cells"] == 5
    names = {t["name"] for t in c.tools()}
    assert {"design_summary", "qor_report", "run_flow", "query_cell"} <= names
    q = json.loads(c.call_tool("compute_qor"))
    assert "hpwl_um" in q
    assert "Congestion" in c.run("report") or "congestion" in c.run("report").lower()
    cell = json.loads(c.call_tool("find_objects", pattern="*", kind="cell", limit=1))["cells"]["names"][0]
    assert json.loads(c.call_tool("query_cell", name=cell))["name"] == cell
    with pytest.raises(RuntimeError):
        c.call_tool("query_cell", name="no_such_cell")
    c.run("place iters=5")  # waits for the job
    assert json.loads(c.summary())["last_flow"]["placement"]["iterations"] >= 0


def test_console_read_only_and_mcp():
    c = hermes.Console(DATA, edits="read-only")
    with pytest.raises(RuntimeError):
        c.call_tool("run_flow")
    with pytest.raises(RuntimeError):
        c.run("place")
    init = json.loads(c.mcp(json.dumps({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})))
    assert init["result"]["serverInfo"]["name"] == "hermes"
    assert c.mcp(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"})) is None
    tools = json.loads(c.mcp(json.dumps({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})))["result"]["tools"]
    assert "run_flow" not in {t["name"] for t in tools}
