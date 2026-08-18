"""
Selectable, tunable prefetcher factory for vector-cache experiments.

Maps a CLI prefetcher name (a key of :data:`PREFETCHERS`) plus a dict of
``--pf-param NAME=VALUE`` overrides to a *zero-arg factory* that builds one
fresh prefetcher SimObject per call.
``VectorSplitCacheHierarchy`` (``vector_cache_hierarchy.py``) calls the
factory once per core, because a single SimObject instance cannot be shared
between caches.

The prefetcher classes themselves live in
``src/mem/cache/prefetch/Prefetcher.py`` (all stock gem5 except this fork's
``vimp``, ``gdp``, ``vtyche`` and ``tyche``); this module only *selects* and
*parameterizes* one from the command line. Every tunable knob is just a
``Param.*`` declared on the chosen class (or inherited from
``QueuedPrefetcher``/``BasePrefetcher``), so any such name is a valid
``--pf-param``. See MEMORY_CONFIG.md for the curated per-prefetcher reference.
"""

from m5.objects import (
    GDPPrefetcher,
    IndirectMemoryPrefetcher,
    IrregularStreamBufferPrefetcher,
    RevelaPrefetcher,
    STeMSPrefetcher,
    StridePrefetcher,
    TychePrefetcher,
    VectorIndirectMemoryPrefetcher,
    VectorTychePrefetcher,
    VectorTyche2Prefetcher,
    VHybridPrefetcher,
)
from m5.params import NULL

# CLI name -> prefetcher class. "none" maps to no prefetcher (NULL).
PREFETCHERS = {
    "none": None,
    "stride": StridePrefetcher,
    "imp": IndirectMemoryPrefetcher,
    "vimp": VectorIndirectMemoryPrefetcher,
    "gdp": GDPPrefetcher,
    "vtyche": VectorTychePrefetcher,
    "vtyche2": VectorTyche2Prefetcher,
    "isb": IrregularStreamBufferPrefetcher,
    "stems": STeMSPrefetcher,
    "tyche": TychePrefetcher,
    "revela": RevelaPrefetcher,
    "vhybrid": VHybridPrefetcher,
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


# Prefetchers whose class default is use_virtual_addresses=True. These train
# on virtual addresses, so the CPU MMU must be registered on the prefetcher
# (BasePrefetcher.registerMMU) or every page-crossing prefetch target is
# silently dropped (Queued::insert requires an MMU to cross a page).
VA_PREFETCHERS = {"vimp", "gdp", "vtyche", "vtyche2", "tyche", "revela",
                  "vhybrid"}

# Prefetchers needing a per-core TycheChainTable wired to both the
# prefetcher (chain_table) and the CPU (tyche_table); see
# src/cpu/tyche_table.hh.
CHAIN_TABLE_PREFETCHERS = {"tyche"}

# Prefetchers needing a per-core VectorChainTable wired to both the
# prefetcher (link_table) and the CPU (vector_chain_table); see
# src/cpu/vector_chain_table.hh.
VECTOR_CHAIN_TABLE_PREFETCHERS = {"gdp", "vtyche", "vtyche2", "vhybrid"}


def needs_chain_table(name):
    """True when the selected prefetcher is fed by the CPU-side
    TycheChainTable channel."""
    return name in CHAIN_TABLE_PREFETCHERS


def needs_vector_chain_table(name):
    """True when the selected prefetcher is fed by the CPU-side
    VectorChainTable channel."""
    return name in VECTOR_CHAIN_TABLE_PREFETCHERS


# Prefetchers needing a per-core RevelaStreamTable wired to both the
# prefetcher (stream_table) and the CPU (revela_table); see
# src/cpu/revela_table.hh.
REVELA_TABLE_PREFETCHERS = {"revela", "vhybrid"}


def needs_revela_table(name, params=None):
    """True when the selected prefetcher is fed by the CPU-side
    RevelaStreamTable channel. vtyche joins only when its opt-in
    announced-limit gate is on (``--pf-param limit_gate=true``), so
    plain vtyche runs keep their table-free wiring (and their
    split-hierarchy side freedom) bit-exactly."""
    params = params or {}
    if name == "vtyche" and bool(_coerce(params.get("limit_gate", 0))):
        return True
    return name in REVELA_TABLE_PREFETCHERS


def needs_mmu(name, params=None):
    """True when the selected prefetcher trains on virtual addresses and
    therefore needs the CPU MMU registered for cross-page translation.

    An explicit ``--pf-param use_virtual_addresses=...`` overrides the
    class default, so PA-mode baselines keep their legacy behavior
    (page-crossing prefetches dropped) unless the user opts in.
    """
    params = params or {}
    raw = params.get("use_virtual_addresses")
    if raw is not None:
        return bool(_coerce(raw))
    return name in VA_PREFETCHERS


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
