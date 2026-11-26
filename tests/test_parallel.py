"""Parallel runtime from Python: threads, background flow jobs, regions."""

import random
import time

import hermes


def make_design(n=3000, seed=1, placed=True):
    rng = random.Random(seed)
    d = hermes.Design("par")
    side = int((n * 500 * 1000 / 0.6) ** 0.5 / 1000 + 1) * 1000
    d.die = hermes.Rect(0, 0, side, side)
    d.row_height = 1000
    for i in range(n):
        c = d.add_cell(f"u{i}", rng.randint(200, 800), 1000)
        if placed:
            d.move_cell(c, hermes.Point(rng.randint(0, side - 1000), rng.randint(0, side - 1000) // 1000 * 1000))
    for k in range(n):
        net = d.add_net(f"n{k}")
        for p in range(rng.randint(2, 4)):
            c = k if p == 0 else min(n - 1, max(0, k + rng.randint(-20, 20)))
            d.connect(c, net, f"P{p}", hermes.Point(100, 500),
                      dir=hermes.PinDir.Output if p == 0 else hermes.PinDir.Input)
    for i in range(4):
        d.move_cell(i, hermes.Point(0 if i % 2 else side - 1000, (i * side // 4) // 1000 * 1000))
        d.set_fixed(i, True)
    return d


def test_threads_and_backend():
    assert hermes.hardware_threads() >= 1
    n = hermes.num_threads()
    hermes.set_num_threads(1)
    assert hermes.num_threads() == 1
    hermes.set_num_threads(n)
    assert "cpu" in hermes.compute_backends()
    assert hermes.compute_backend() == "cpu"


def test_flow_job_progress_and_snapshots():
    d = make_design(placed=False)
    pub = hermes.SnapshotPublisher(d)
    opts = hermes.FlowOptions()
    opts.placer.max_iterations = 100
    job = hermes.run_flow_async(d, opts, publish_interval_s=0.01)
    seen = set()
    while not job.wait(timeout=0.01):
        s = pub.latest()          # GUI-style read while the job runs
        seen.add(s.version)
        assert 0.0 <= job.progress().fraction <= 1.0
    rep = job.result()
    assert job.state == hermes.JobState.Done
    assert not rep.cancelled and rep.placement.iterations > 0
    assert job.progress().fraction == 1.0
    assert len(seen) >= 1


def test_flow_job_cancel():
    d = make_design(n=6000, placed=False)
    opts = hermes.FlowOptions()
    opts.placer.max_iterations = 100000
    opts.placer.stop_overflow = 0.0
    job = hermes.run_flow_async(d, opts)
    t0 = time.time()
    while job.progress().updates < 3 and time.time() - t0 < 30:
        time.sleep(0.005)
    job.cancel()
    assert job.wait(timeout=30)
    assert job.state == hermes.JobState.Cancelled
    assert job.result().cancelled


def test_regions_local_and_loopback():
    a = make_design(seed=3)
    b = make_design(seed=3)
    grid = hermes.region.RegionGrid(a.die, 3, 3)
    pa = hermes.region.Partition(a, grid)
    pb = hermes.region.Partition(b, grid)
    assert pa.num_regions == 9
    assert sum(len(pa.cells(r)) for r in range(9)) == a.num_cells
    assert sorted(pa.spatial_order()) == list(range(a.num_cells))
    before = a.total_hpwl()
    sa = hermes.region.run_regions(a, pa, pa.regions_of_color(0))
    sb = hermes.region.run_regions(b, pb, pb.regions_of_color(0), executor="loopback", workers=3)
    assert sa.cells_moved == sb.cells_moved > 0
    assert sa.hpwl_after < sa.hpwl_before
    assert a.total_hpwl() < before
    assert [a.cell(i).loc.x for i in range(a.num_cells)] == [b.cell(i).loc.x for i in range(b.num_cells)]
    assert "median" in hermes.region.solvers()
