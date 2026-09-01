# AccessPort catalog

The firmware carries a small, fixed table mapping AccessPort **part numbers**
to vehicle families — `AP3-SUB-004` to Subaru, `AP3-FOR-001` to Ford, and so
on. 64 parts across 12 families.

## What it is for

Safety, not features. RevLink Sidecar refuses to treat an unrecognised USB
device as an AccessPort. The catalog is how it decides, using the exact part
number returned by the read-only identity handshake — never a guess from the
USB VID/PID, and never a prefix match against an unknown part.

A part that is not in the table is reported as unrecognised. Nothing is
inferred about it, and no proprietary request is sent to it.

## Where the data comes from

Part numbers are **publicly published product identifiers**. They appear on
retail listings, product packaging, and vendor documentation, in the same way
any manufacturer's SKU does. The mapping from a SKU prefix to a car brand is
an observable fact about how those SKUs are named.

The table was assembled by hand from those published identifiers and reviewed
entry by entry before being committed.

## What it is not

This table contains **no vendor software, firmware, calibration data, map
content, or protected material of any kind**. It is a list of product codes
and the car brand each one is sold for.

Nothing in it — or in this firmware — decodes, unlocks, redistributes, or
circumvents any vendor's software or protected map format. See
[`../NOTICE`](../NOTICE).

## Revision

The table is stamped with a revision string reported by the portal's status
endpoint so a running Sidecar can be traced to an exact table:

```c
#define REVLINK_ACCESSPORT_CATALOG_REVISION "revlink-catalog-2026-07-28"
```

Bump it in
[`revlink_accessport_catalog.h`](../firmware/components/accessport_catalog/include/revlink_accessport_catalog.h)
whenever an entry changes.

## Adding a part

1. Confirm the exact part number as published by the vendor. Do not guess from
   a prefix, and do not extrapolate a range.
2. Add the entry to `PARTS[]` in
   [`revlink_accessport_catalog.c`](../firmware/components/accessport_catalog/revlink_accessport_catalog.c),
   keeping the array **sorted by part number** — lookup is a bounded binary
   search and assumes sort order.
3. Add the family to `FAMILIES[]` if it is new.
4. Bump `REVLINK_ACCESSPORT_CATALOG_REVISION`.
5. Run `./scripts/ci-local.sh`; `catalog_host_test` checks sort order,
   bounds, and lookup behaviour.

If you have hardware that reports a part number this table does not know,
please open an issue with the exact string. That is more useful than a guess,
and it is the only way the list grows accurately.

## Related

- [`SUPPORTED_ACCESSPORTS.md`](SUPPORTED_ACCESSPORTS.md) — the identity
  handshake and the safety rules around it
- [`SINGLE_ACCESSPORT_SAFETY.md`](SINGLE_ACCESSPORT_SAFETY.md) — why two
  attached devices fail closed
