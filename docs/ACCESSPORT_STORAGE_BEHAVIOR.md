# AccessPort storage behavior

This document records the storage findings that affect RevLink's datalog and
map workflows. Keep protocol facts, live observations, and firmware-research
claims separate so the UI does not present an estimate as a device-reported
setting.

## Datalogs

Live RevLink indexing has verified that current AccessPort firmware stores new
datalogs as gzip-compressed CSV files (`.csv.gz`). RevLink normalizes those
files to ordinary CSV at the local-cache boundary while retaining support for
legacy plain `.csv` files.

The uploaded firmware research indicates:

- manual, automatic, and triggered capture modes use native compressed log
  families;
- native numbered `.csv.gz` logs rotate by filename number, not by a recorded
  wall-clock timestamp;
- the oldest numbered matching file is deleted when the configured datalog
  count is reached;
- the common firmware default is 15, but the setting is adjustable on the
  AccessPort and therefore must not be presented as a universally detected
  limit;
- zero-byte matching logs are removed automatically; and
- renamed files that do not match the firmware's numbered pattern may not
  participate in automatic rotation.

Until RevLink can read the active `datalog_file_count_limit` setting directly,
the UI shows the indexed native compressed-log count, explains FIFO rotation,
and labels 15 as a common default rather than the selected device's confirmed
limit.

## Maps

The uploaded firmware research indicates that maps do not rotate. The
AccessPort displays up to 100 maps and refuses additional maps when that limit
is reached. RevLink therefore shows live map-slot usage while the selected
AccessPort is connected and warns as the count approaches 100.

This limit should be revalidated against each supported AccessPort family and
firmware line before RevLink claims universal compatibility.

## Device deletion and local backup

RevLink exposes device deletion only for files directly under `datalog/` and
`maps/`. It remains protected by both write gates:

1. the installation administrator must make device writes available; and
2. the signed-in user must accept the warning and enable writes for their own
   account or temporary demo session.

The UI never asks the user to retype a path. It shows a destructive-action
confirmation and the file's backup state:

- **Backed up locally:** deleting removes the device copy while the active
  dataset keeps its cached copy.
- **Not backed up:** local backup is selected by default. RevLink downloads and
  caches the file before sending the delete command.
- **Delete without backup:** allowed only after the explicit destructive
  acknowledgement and described as permanent loss from the device.

If the AccessPort is physically attached but not connected, RevLink offers the
same temporary `connect → perform action → disconnect` workflow used by other
device writes.

Cloud backup is not implemented. The UI describes it as coming later and never
implies that an uncached file exists anywhere else.

## Validation still required

- Read the active datalog rotation limit from a proven device setting or
  response rather than relying on the common default.
- Revalidate the 100-map limit across supported part numbers and firmware.
- Complete the controlled Linux delete acceptance using only the established
  round-trip test map. Never validate deletion against a customer map or log.
