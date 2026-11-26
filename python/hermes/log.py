"""Hermes logging (``hermes.log``).

Channels are dot-separated (``route``, ``route.tree``, ``place.gp``); a channel
without its own level follows its parent, and ``*`` is the root (default info).
Messages with ids (``DEF-0003``) can be switched off or capped.

    import hermes.log as hl
    hl.configure("warn,route=debug,route.tree=trace,msg:DEF-0003=off")
    hl.set_level("place.gp", "verbose")
    hl.set_message_cap("GRT-0001", 5)
    with hl.capture() as recs:
        hermes.run_flow(d)
    print([r.text for r in recs])
"""

import atexit as _atexit
import contextlib as _contextlib

from . import _hermes

_native = _hermes.log
globals().update({k: getattr(_native, k) for k in dir(_native) if not k.startswith("__")})


@_contextlib.contextmanager
def capture(level="trace", console=True):
    """Collect records emitted inside the block into a list.

    ``console=False`` also silences the console while the block runs.
    """
    records = []
    sink = _native.add_callback_sink(records.append, level)
    old = _native.console_level()
    if not console:
        _native.set_console_level("off")
    try:
        yield records
    finally:
        _native.remove_sink(sink)
        if not console:
            _native.set_console_level(old)


def to_python_logging(logger_name="hermes", level="trace"):
    """Forward hermes records to the standard :mod:`logging` module.

    Returns the sink id (pass it to :func:`remove_sink` to stop forwarding).
    Pair with ``set_console_level("off")`` to avoid printing twice.
    """
    import logging

    py_levels = {
        Level.Error: logging.ERROR,
        Level.Warn: logging.WARNING,
        Level.Info: logging.INFO,
        Level.Verbose: logging.INFO - 5,
        Level.Debug: logging.DEBUG,
        Level.Trace: logging.DEBUG - 5,
    }
    base = logging.getLogger(logger_name)

    def forward(r):
        name = r.channel if r.channel != "*" else ""
        lg = base.getChild(name) if name else base
        text = f"[{r.id}] {r.text}" if r.id else r.text
        lg.log(py_levels.get(r.level, logging.INFO), text)

    return _native.add_callback_sink(forward, level)


# Python callables must not be called after the interpreter starts shutting down.
_atexit.register(_native._remove_python_sinks)
