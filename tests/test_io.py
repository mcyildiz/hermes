import os
import shutil

import pytest

import hermes
from hermes import io

DATA = os.path.join(os.path.dirname(__file__), "data")


def lefdef_dir(tmp_path):
    d = tmp_path / "in"
    d.mkdir()
    shutil.copy(os.path.join(DATA, "sample.lef"), d / "cells.lef")
    shutil.copy(os.path.join(DATA, "sample.def"), d / "top.def")
    return d


def test_directory_input_and_formats(tmp_path):
    src = lefdef_dir(tmp_path)
    plan = io.plan_input(str(src))
    assert plan.format == "lefdef"
    assert [i.role for i in plan.items] == ["lef", "def"]
    d, info = io.load_input(str(src))
    assert d.num_cells == 5 and info["format"] == "lefdef"
    assert d.dbu_per_micron > 0
    assert any(d.cell(c).placed for c in range(d.num_cells))
    assert d.cell_master(0) != ""
    assert {name for name, _ in io.readers()} >= {"lefdef", "bookshelf", "hdb"}
    assert {name for name, _ in io.writers()} >= {"hdb", "def", "pl", "bookshelf"}


def test_hdb_roundtrip(tmp_path):
    d, _ = io.load_input(str(lefdef_dir(tmp_path)))
    path = str(tmp_path / "x.hdb")
    rep = io.save_design(d, path, note="pytest", extra={"state": b"\x01\x02"})
    assert rep["file_bytes"] > 0 and io.is_design_file(path)
    e, extra = io.load_design(path)
    assert extra == {"state": b"\x01\x02"}
    assert e.num_cells == d.num_cells and e.total_hpwl() == d.total_hpwl()
    assert e.cell_name(0) == d.cell_name(0) and e.cell_master(0) == d.cell_master(0)
    assert len(e.rows) == len(d.rows)
    info = io.inspect_design_file(path)
    assert info["version"][0] == 1 and info["meta"]["note"] == "pytest"
    # A directory holding only an .hdb loads too.
    f, _ = io.load_input(path)
    assert f.num_nets == d.num_nets


def test_writers_and_bookshelf(tmp_path):
    d, _ = io.load_input(str(lefdef_dir(tmp_path)))
    io.write_design(d, str(tmp_path / "out.def"))
    e = io.read_lef_def([os.path.join(DATA, "sample.lef")], str(tmp_path / "out.def"))
    assert e.total_hpwl() == pytest.approx(d.total_hpwl())
    aux = io.write_bookshelf(d, str(tmp_path / "bs"))
    b = io.read_bookshelf(aux)
    assert b.num_cells == d.num_cells
    b2, info = io.load_input(str(tmp_path / "bs"), {"bookshelf.scale": 1})
    assert info["format"] == "bookshelf" and b2.num_nets == b.num_nets
    io.write_bookshelf_pl(d, str(tmp_path / "x.pl"))
    assert (tmp_path / "x.pl").read_text().startswith("UCLA pl")


def test_errors(tmp_path):
    empty = tmp_path / "empty"
    empty.mkdir()
    with pytest.raises(io.InputError):
        io.plan_input(str(empty))
    bad = tmp_path / "bad.hdb"
    bad.write_bytes(b"HERMESDB" + b"\0" * 10)
    with pytest.raises(io.ParseError):
        io.load_design(str(bad))


def test_placer_keep_placed_option():
    o = hermes.PlacerOptions()
    assert o.keep_placed
    o.keep_placed = False
    f = hermes.FlowOptions()
    f.run_routing = False
