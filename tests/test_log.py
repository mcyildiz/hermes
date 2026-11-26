import json
import logging

import pytest

import hermes
import hermes.log as hl
from test_python import build_design


@pytest.fixture(autouse=True)
def reset_log():
    yield
    for c in hl.channels():
        if c["name"] != "*":
            hl.clear_level(c["name"])
    hl.set_level("*", "info")
    hl.set_default_message_cap(None)
    hl.reset_message_counts()
    hl.set_console_level("trace")


def test_levels_and_hierarchy():
    hl.set_level("py.route", "debug")
    assert hl.level("py.route.tree") == hl.Level.Debug
    hl.set_level("py.route.tree", hl.Level.Off)
    assert hl.level("py.route.tree") == hl.Level.Off
    assert hl.explicit_level("py.route.tree") == hl.Level.Off
    hl.clear_level("py.route.tree")
    assert hl.level("py.route.tree") == hl.Level.Debug
    assert hl.enabled("py.route.tree", "debug")
    assert not hl.enabled("py.other", "debug")
    with pytest.raises(ValueError):
        hl.set_level("x", "loud")
    with pytest.raises(ValueError):
        hl.configure("x=loud")


def test_capture_flow_messages():
    hl.set_level("place.gp", "debug")
    d = build_design()
    opts = hermes.FlowOptions()
    opts.placer.max_iterations = 20
    opts.router.grid_x = opts.router.grid_y = 16
    with hl.capture(console=False) as recs:
        report = hermes.run_flow(d, opts)
    chans = {r.channel for r in recs}
    assert {"flow", "place.gp", "route.global"} <= chans
    iters = [r for r in recs if r.channel == "place.gp" and r.text.startswith("iter")]
    assert len(iters) == report.placement.iterations  # every iteration at Debug


def test_quiet_channel():
    hl.set_level("place", "off")
    hl.set_level("flow", "warn")
    with hl.capture(console=False) as recs:
        hermes.run_flow(build_design())
    assert not any(r.channel.startswith("place") or r.channel == "flow" for r in recs)


def test_message_cap_and_off():
    hl.set_message_cap("PY-0001", 2)
    with hl.capture(console=False) as recs:
        for i in range(5):
            hl.log("py", "warn", f"w{i}", id="PY-0001")
    assert [r.text for r in recs] == ["w0", "w1"]
    assert recs[-1].last_of_id
    assert hl.message_count("PY-0001") == 2
    hl.configure("msg:PY-0002=off")
    with hl.capture(console=False) as recs:
        hl.log("py", "error", "x", id="PY-0002")
    assert recs == []
    ids = {m["id"]: m for m in hl.messages()}
    assert ids["PY-0001"]["cap"] == 2 and not ids["PY-0002"]["enabled"]
    hl.set_message_cap("PY-0001", None)


def test_file_and_json_sinks(tmp_path):
    txt, js = tmp_path / "run.log", tmp_path / "run.jsonl"
    a = hl.add_file_sink(str(txt))
    b = hl.add_json_sink(str(js), level="warn")
    hl.set_console_level("off")
    hl.log("py.io", "info", "hello")
    hl.log("py.io", "warn", 'quote " here', id="PY-0010")
    assert hl.remove_sink(a) and hl.remove_sink(b)
    lines = txt.read_text().splitlines()
    assert len(lines) == 2 and lines[0].endswith("[INFO py.io] hello")
    rec = [json.loads(line) for line in js.read_text().splitlines()]
    assert len(rec) == 1 and rec[0]["msg"] == 'quote " here' and rec[0]["id"] == "PY-0010"


def test_python_logging_bridge(caplog):
    sink = hl.to_python_logging()
    hl.set_console_level("off")
    try:
        with caplog.at_level(logging.DEBUG, logger="hermes"):
            hl.log("py.bridge", "warn", "bridged", id="PY-0020")
    finally:
        hl.remove_sink(sink)
    assert any(r.name == "hermes.py.bridge" and r.message == "[PY-0020] bridged" and
               r.levelno == logging.WARNING for r in caplog.records)


@pytest.mark.filterwarnings("ignore::pytest.PytestUnraisableExceptionWarning")
def test_callback_exception_does_not_break_logging():
    def bad(_):
        raise RuntimeError("boom")

    sink = hl.add_callback_sink(bad)
    try:
        with hl.capture(console=False) as recs:
            hl.log("py", "info", "still delivered")
    finally:
        hl.remove_sink(sink)
    assert [r.text for r in recs] == ["still delivered"]
