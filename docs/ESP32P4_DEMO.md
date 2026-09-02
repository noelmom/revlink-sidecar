# RevLink Sidecar ESP32-P4 demo

A runbook for recording or giving a live demonstration. Everything runs on the
Sidecar and its microSD card; nothing needs a network beyond the one the phone
and the Sidecar share.

## Before recording

1. Confirm a recent verified microSD backup exists after the latest meaningful
   sync. Follow [`SIDECAR_SD_BACKUP.md`](SIDECAR_SD_BACKUP.md) before any
   storage or recovery demonstration.
2. Power the ESP32-P4 and confirm the OLED reaches its ready state.
3. Attach exactly one AccessPort directly to the board's USB host port. Until
   the beta gate in
   [`SINGLE_ACCESSPORT_SAFETY.md`](SINGLE_ACCESSPORT_SAFETY.md) passes, do not
   demo through a hub that can expose more than one AccessPort.
4. Put the phone, tablet, or in-dash browser on the same Wi-Fi network.
5. Open `http://revlink-<last-8-mac>.local`.
6. If mDNS is unavailable on the client, use the IPv4 address shown during
   connection setup.
7. Writes and deletes both ship and both start locked. Leave them locked
   unless the presentation includes a rehearsed segment for them, and say on
   camera that they are separate consents the owner turns on.

The current development board is:

```text
hostname: revlink-<last-eight-mac>.local
fallback: use the DHCP address shown by the current network when mDNS is unavailable
```

The IPv4 address is DHCP-assigned and can change. The hostname is the stable
demo address.

## Suggested two-minute flow

1. Start on Dashboard and show the connected vehicle, AccessPort part,
   firmware, local cache count, and last synchronization.
2. Trigger **Sync AccessPort**. Explain that RevLink verifies the local cache
   and downloads only new or changed files.
3. Open **Datalogs**. Sort by Newest and point out **Initial sync** timestamps,
   including the filename wrap from `datalog58` to `datalog1` and `datalog2`.
4. Open a datalog by selecting its filename.
5. Show **Single** view, search for channels, and switch the Y-axis between
   shared values and per-channel ranges.
6. Show **Split** view and switch between the Air/Fuel, Boost,
   Knock/Roughness, Timing, and Thermal presets.
7. Use **Print / PDF** to show the report-ready chart path, then close the
   viewer.
8. Add a note to a datalog, open **Maps**, and show the cached calibration.
9. Open **Vehicle Health Beta**, analyze the latest 10 logs, and scrub the
   deterministic evidence timeline. Show health and confidence separately.
10. Select up to five datalogs and choose **Email selected**. On a browser that
    supports native file sharing, choose Mail and show the individual CSV
    attachments plus the prepared subject and message. The compatibility
    fallback downloads the CSVs and opens the default mail draft.
11. Show that direct downloads are individual files, not a ZIP archive.
12. End on **Device** or Dashboard with the AccessPort identity and local-only
    storage summary visible.

## Demo claims that are accepted

- USB 2.0 high-speed enumeration with 512-byte bulk packets;
- true AccessPort identity and firmware discovery;
- multi-device storage namespaces;
- bounded incremental datalog and map backup;
- SHA-256 verification and immutable cached objects;
- filename-wrap-safe Initial sync chronology;
- version-scoped notes;
- direct CSV/map downloads;
- native OS sharing of up to five CSV datalogs with a prepared tuner-friendly
  subject and message, plus a mailto/download fallback;
- on-device Datazap-style CSV visualization;
- deterministic Vehicle Health trends with attributable source logs,
  confidence, and map provenance;
- guarded, identity-pinned map transfer with byte-identical read-back when the
  owner explicitly enables writes;
- startup-screen and screenshot backup plus a reusable, per-device image
  library;
- software session close and return to normal AccessPort operation; and
- Wi-Fi client or fallback-hotspot access.

## Do not claim yet

- production authentication or public-internet security;
- cloud backup, share links, or Sidecar-hosted email delivery;
- embedded AI;
- a live-accepted startup-screen write from the P4;
- a second AccessPort arriving during a write or delete; or
- ECU flashing or real-time tuning.

## Recovery

If the friendly hostname does not open:

1. confirm the client and Sidecar are on the same Wi-Fi network;
2. wait for the OLED/network state to settle;
3. try the current IPv4 address;
4. reconnect through the fallback hotspot if the preferred network is absent;
5. reload the page only after the network transition completes.

Do not remove power during a running synchronization. A normal completed
session closes the AccessPort file mode and releases the USB interface.
