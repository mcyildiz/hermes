import pytest

import hermes


def build_design():
    d = hermes.Design("pytest")
    d.die = hermes.Rect(0, 0, 10000, 10000)
    d.row_height = 100
    anchor = d.add_cell("io", 100, 100)
    d.move_cell(anchor, hermes.Point(0, 0))
    d.set_fixed(anchor, True)
    macro = d.add_cell("ram", 2000, 2000, is_macro=True)
    u = d.add_cell("u0", 200, 100)
    n0 = d.add_net("n0")
    d.connect(anchor, n0, "Z", dir=hermes.PinDir.Output)
    d.connect(u, n0, "A")
    n1 = d.add_net("n1")
    d.connect(u, n1, "Z", dir=hermes.PinDir.Output)
    d.connect(macro, n1, "D")
    # Spread the movable objects so the starting HPWL is non-trivial.
    d.move_cell(macro, hermes.Point(7000, 7000))
    d.move_cell(u, hermes.Point(9000, 9000))
    return d


def test_design_basics():
    d = build_design()
    assert d.num_cells == 3
    assert d.num_nets == 2
    assert d.num_macros == 1


def test_flow():
    d = build_design()
    opts = hermes.FlowOptions()
    opts.placer.max_iterations = 30
    opts.router.grid_x = opts.router.grid_y = 16
    before = d.total_hpwl()
    report = hermes.run_flow(d, opts)
    assert 0 < report.placement.iterations <= 30
    assert report.placement.hpwl < before
    assert report.global_route.wirelength > 0
    assert d.cell(1).fixed  # macro fixed after macro legalization


def test_names():
    d = build_design()
    assert d.cell_name(1) == "ram"
    assert d.find_cell("u0") == 2
    assert d.find_cell("nope") is None
    assert d.net_name(d.find_net("n1")) == "n1"
    pin = d.find_pin(2, "Z")
    assert pin is not None and d.pin_name(pin) == "Z"
    assert d.find_pin(2, "D") is None
    assert d.find_pin(None, "Z") is None
    io = d.connect(-1, 0, "pad_in")
    assert d.find_pin(None, "pad_in") == io
    with pytest.raises(ValueError):
        d.add_cell("u0", 10, 10)
    with pytest.raises(ValueError):
        d.add_net("n0")


def test_edits_are_read_only_values():
    d = build_design()
    c = d.cell(2)
    with pytest.raises(AttributeError):
        c.loc = hermes.Point(1, 1)  # edit through the design instead
    d.move_cell(2, hermes.Point(1, 2))
    assert d.cell(2).loc.x == 1 and c.loc.x == 9000  # c is a copy
    pin = d.find_pin(2, "Z")
    assert d.pin(pin).net == d.find_net("n1") and d.pin(pin).cell == 2
    assert d.net_pins(d.find_net("n0")) == [0, 1]
    d.reconnect_pin(pin, d.find_net("n0"))
    assert d.net_pins(d.find_net("n0")) == [0, 1, pin]
    d.remove_cell(1)
    assert d.cell(1).removed and d.find_cell("ram") is None and d.num_macros == 0


def test_transaction_and_changes():
    d = build_design()
    assert not d.tracking  # off by default; opt in for incremental work
    d.tracking = True
    d.flush()
    before = [d.cell(i).loc.x for i in range(d.num_cells)]
    with d.transaction() as tx:
        d.move_cell(2, hermes.Point(5, 5))
        d.add_cell("tmp", 10, 10)
        assert len(tx) == 2
        tx.rollback()
    assert [d.cell(i).loc.x for i in range(d.num_cells)] == before and d.num_cells == 3
    with pytest.raises(ValueError):
        with d.transaction():
            d.move_cell(2, hermes.Point(6, 6))
            raise ValueError("boom")  # rolled back
    assert d.cell(2).loc.x == before[2]
    with d.transaction():
        d.move_cell(2, hermes.Point(7, 7))  # committed
    assert d.cell(2).loc.x == 7 and not d.in_transaction

    seen = []

    class Listener(hermes.DesignListener):
        def on_flush(self, design, changes):
            seen.append((changes.version, changes.cells, changes.nets, design.num_cells))

    d.subscribe(Listener())
    d.flush()  # the rollback + commit above
    d.move_cell(0, hermes.Point(1, 1))
    assert d.pending.cells == [0]
    v = d.flush()
    assert seen[-1] == (v, [0], [0], 3)
    # Listeners also fire from engine threads (GIL released in run_flow).
    opts = hermes.FlowOptions()
    opts.placer.max_iterations = 10
    hermes.run_flow(d, opts)
    assert seen[-1][0] == d.version and len(seen) > 3


def test_snapshot_and_incremental_hpwl():
    d = build_design()
    d.tracking = True
    pub = hermes.SnapshotPublisher(d)
    s1 = pub.latest()
    d.move_cell(2, hermes.Point(100, 100))
    d.flush()
    s2 = pub.latest()
    assert s2.version == s1.version + 1
    assert s1.cell(2).loc.x == 9000 and s2.cell(2).loc.x == 100
    assert s2.cell_name(2) == "u0" and s2.find_cell("ram") == 1

    boxes = hermes.NetBoxCache(d)
    assert boxes.total_hpwl() == d.total_hpwl()
    d.move_cell(1, hermes.Point(0, 0))
    d.flush()
    assert boxes.total_hpwl() == d.total_hpwl() and boxes.last_refresh_count == 1
