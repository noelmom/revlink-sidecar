# Supported AccessPort catalog

RevLink Sidecar identifies an AccessPort from the exact part number returned by
the authoritative, read-only identity handshake. The product firmware contains
a bounded catalog of 64 reviewed parts across 12 families. It does not infer a
specific vehicle from a part-number prefix or from USB VID/PID alone.

The table is assembled from vendor-published product identifiers and contains
no vendor software, firmware, calibration, or map content. Its contents,
provenance, and update procedure are documented in
[`ACCESSPORT_CATALOG.md`](ACCESSPORT_CATALOG.md).

- firmware catalog revision: `revlink-catalog-2026-07-28`

## Safety model

1. USB enumeration must first match the capture-verified AccessPort transport:
   VID/PID `1a84:0121`, interface `0`, bulk OUT `0x03`, bulk IN `0x82`, high
   speed, and 512-byte maximum packets.
2. RevLink performs the four read-only identity mini requests.
3. The exact returned part number is matched case-sensitively against the
   catalog below.
4. The vehicle displayed to the user is the identity response's own vehicle
   string. A part can support many vehicles, so RevLink never guesses the
   installed vehicle from the family catalog.
5. A recognized part may use the universal, read-only file identity/list/pull
   protocol.
6. An unknown, malformed, differently cased, prefixed, or suffixed part remains
   visible for troubleshooting, but namespace creation, listing, downloads,
   and synchronization fail closed.
7. Live monitoring, map interpretation, firmware updates, uploads, and deletes
   are separate capabilities. This catalog does not enable them.

This separation allows new parts to be reviewed and added without weakening
the USB transport or silently granting unrelated features.

## Reviewed parts

| Family | Count | Exact part numbers |
|---|---:|---|
| AccessPort Activation (`APA`) | 1 | `AP3-APA-001` |
| Australian Subaru (`AU`) | 3 | `AP3-AU-SUB-003`, `AP3-AU-SUB-004`, `AP3-AU-SUB-006` |
| BMW (`BMW`) | 2 | `AP3-BMW-001`, `AP3-BMW-002` |
| Ford (`FOR`) | 14 | `AP3-FOR-001`, `AP3-FOR-002`, `AP3-FOR-003`, `AP3-FOR-004`, `AP3-FOR-005`, `AP3-FOR-006`, `AP3-FOR-007`, `AP3-FOR-008`, `AP3-FOR-009`, `AP3-FOR-010`, `AP3-FOR-011`, `AP3-FOR-012`, `AP3-FOR-013`, `AP3-FOR-014` |
| Ford Performance (`FRP`) | 1 | `AP3-FRP-001` |
| Honda (`HON`) | 4 | `AP3-HON-001`, `AP3-HON-002`, `AP3-HON-003`, `AP3-HON-004` |
| Mazda (`MAZ`) | 1 | `AP3-MAZ-002` |
| Mitsubishi (`MIT`) | 1 | `AP3-MIT-002` |
| Nissan (`NIS`) | 4 | `AP3-NIS-005`, `AP3-NIS-006`, `AP3-NIS-007`, `AP3-NIS-008` |
| Porsche (`POR`) | 20 | `AP3-POR-001`, `AP3-POR-002`, `AP3-POR-003`, `AP3-POR-004`, `AP3-POR-005`, `AP3-POR-006`, `AP3-POR-007`, `AP3-POR-008`, `AP3-POR-009`, `AP3-POR-010`, `AP3-POR-011`, `AP3-POR-012`, `AP3-POR-013`, `AP3-POR-014`, `AP3-POR-015`, `AP3-POR-016`, `AP3-POR-018`, `AP3-POR-019`, `AP3-POR-020`, `AP3-POR-021` |
| Subaru (`SUB`) | 7 | `AP3-SUB-001`, `AP3-SUB-002`, `AP3-SUB-003`, `AP3-SUB-004`, `AP3-SUB-005`, `AP3-SUB-006`, `AP3-SUB-007` |
| Volkswagen / Audi (`VLK`) | 6 | `AP3-VLK-001`, `AP3-VLK-002`, `AP3-VLK-003`, `AP3-VLK-004`, `AP3-VLK-005`, `AP3-VLK-006` |

## Updating the catalog

Treat a catalog update as a safety-policy change:

1. confirm the exact part number as published by the vendor — never a guess
   from a prefix and never an extrapolated range;
2. add exact parts to `revlink_accessport_catalog.c`;
3. keep the table uniquely sorted;
4. update `REVLINK_ACCESSPORT_CATALOG_REVISION` and the expected count;
5. run the host catalog test, complete local CI, and an identity-only hardware
   check before enabling sync for the new part.

Do not add wildcard matching. Do not equate a family with a specific vehicle.
Do not turn catalog membership into permission for writes or live ECU access.
