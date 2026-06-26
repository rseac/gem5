"""
Selectable, tunable prefetcher factory for vector-cache experiments.

Maps a CLI prefetcher name (``none``/``stride``/``imp``/``isb``/``stems``) plus
a dict of ``--pf-param NAME=VALUE`` overrides to a *zero-arg factory* that
builds one fresh prefetcher SimObject per call. ``VectorSplitCacheHierarchy``
(``vector_cache_hierarchy.py``) calls the factory once per core, because a
single SimObject instance cannot be shared between caches.

The prefetcher classes themselves are stock gem5
(``src/mem/cache/prefetch/Prefetcher.py``); this module only *selects* and
*parameterizes* one from the command line. Every tunable knob is just a
``Param.*`` declared on the chosen class (or inherited from
``QueuedPrefetcher``/``BasePrefetcher``), so any such name is a valid
``--pf-param``. See MEMORY_CONFIG.md for the curated per-prefetcher reference.
"""

from m5.objects import (
    IndirectMemoryPrefetcher,
    IrregularStreamBufferPrefetcher,
    STeMSPrefetcher,
    StridePrefetcher,
)
from m5.params import NULL

# CLI name -> prefetcher class. "none" maps to no prefetcher (NULL).
PREFETCHERS = {
    "none": None,
    "stride": StridePrefetcher,
    "imp": IndirectMemoryPrefetcher,
    "isb": IrregularStreamBufferPrefetcher,
    "stems": STeMSPrefetcher,
}


def _coerce(value):
    """Coerce a raw ``--pf-param`` string to the type its Param expects.

    The rule ``int -> bool -> str`` covers the three value kinds gem5's
    tunable prefetcher params use: ``Param.Unsigned``/``Param.Int`` -> int,
    ``Param.Bool`` -> bool, ``Param.MemorySize`` -> str (e.g. ``"2KiB"``).
    ``Param.MemorySize`` also accepts a plain int, so an int-looking size
    still works.
    """
    try:
        return int(value)
    except (TypeError, ValueError):
        pass
    if value in ("True", "true"):
        return True
    if value in ("False", "false"):
        return False
    return value


def build(name, params=None):
    """Return a zero-arg factory ``() -> fresh prefetcher SimObject``.

    ``name`` is a key of :data:`PREFETCHERS`; ``params`` is a dict of
    ``--pf-param`` name -> raw string value. For ``"none"`` the factory
    returns ``NULL`` (no prefetcher).

    Raises ``ValueError`` for an unknown prefetcher name, or an unknown
    parameter name (with the valid names listed), so a CLI typo fails with a
    clear message instead of gem5's lower-level "object has no attribute"
    error. This validation is the single place to add or relax a per-prefetcher
    allowlist.
    """
    params = params or {}
    if name not in PREFETCHERS:
        valid = ", ".join(sorted(PREFETCHERS))
        raise ValueError(f"unknown --prefetcher '{name}'; valid: {valid}")

    cls = PREFETCHERS[name]
    if cls is None:  # "none" -> no prefetcher
        return lambda: NULL

    coerced = {}
    for key, raw in params.items():
        # cls._params is a multidict that chains to inherited params, so this
        # accepts knobs from QueuedPrefetcher/BasePrefetcher too (e.g.
        # queue_size, prefetch_on_access).
        if key not in cls._params:
            valid = ", ".join(sorted(cls._params.keys()))
            raise ValueError(
                f"unknown --pf-param '{key}' for prefetcher '{name}'; "
                f"valid: {valid}"
            )
        coerced[key] = _coerce(raw)

    # Fresh instance per call: caches cannot share a SimObject.
    return lambda: cls(**coerced)
