# Final-product network architecture requirements

This document records requirements to review before the production RevLink
hardware and firmware architecture is finalized. The ESP32-P4/ESP32-C6
prototype is the active product path and already implements the bounded
client-first/fallback-hotspot coordinator described here. An earlier
Raspberry Pi reference build used a permanent single-purpose hotspot instead;
do not copy that two-radio prototype assumption into the Sidecar.

## Product baseline

The ESP32-P4 product uses its single ESP32-C6 radio in one primary Wi-Fi mode
at a time. The initial product must not depend on simultaneous access-point
and station operation. Client mode is preferred when a saved network is
reachable; the device hotspot is the deterministic onboarding and recovery
path. Simultaneous AP+STA may be evaluated later, but only as an enhancement
after coexistence, channel, throughput, power, and failure-recovery testing.

The platform-neutral `revlink_network` coordinator now codifies this policy
without depending on ESP-IDF, ESP-Hosted, a credential store, HTTP, or BLE:

```text
boot
  -> search saved networks
      -> connect selected network -> client ready
      -> none/failure/timeout     -> hotspot ready

client link lost
  -> bounded reconnect to the same network
      -> client ready
      -> failure/timeout -> hotspot ready

client reports connected but local data path is stale
  -> probe the DHCP gateway while no transfer is active
      -> three consecutive successes absent -> restart radio + reconnect
      -> reconnect timeout -> hotspot ready
      -> unrecoverable radio/hotspot fault -> one guarded device reboot
```

Network records are opaque identifiers in this layer. SSIDs and credentials
remain owned by a secure storage adapter and must never enter policy logs.
User-requested mode changes are rejected while a file transfer is active.

The first P4/C6 runtime adapter was hardware-accepted on 2026-07-27. This
identity-v1 milestone was later superseded by the last-eight identity described
below. With no
saved station credential, the coordinator deterministically selected fallback
mode, waited for a RAM-only WPA2 credential, and started the onboard C6 as a
private SoftAP. The original generated identity used
`RevLink-<last six AP-MAC digits>`, the ESP-IDF AP netif supplied DHCP, and
the coordinator reported `hotspot-ready`. No credential was persisted or
logged, AccessPort writes remained disabled, and the attached AccessPort
continued to enumerate at USB high speed.

The local onboarding slice was hardware-accepted later on 2026-07-27. It
generates a new hotspot password from the hardware RNG at every boot, shows
the password only on the physical OLED, and never writes it to a log, NVS, or
microSD. The C6 provides DHCP on `192.168.4.1`; bounded captive
DNS, an HTTP setup page, captive-probe routes, and the unique
`revlink-<MAC suffix>.local` mDNS name run only as local adapters. Submitted
station credentials are validated, copied into the runtime, and zeroized from
request buffers. The runtime persists a candidate only after a successful
association, so a bad password cannot replace the last working network. On a
failed join the coordinator restores the same hotspot and the OLED keeps its
RAM-only password visible. Network changes remain blocked during a file
transfer. The development record uses versioned NVS storage; shipping hardware
must add encrypted NVS/flash encryption and secure boot.

The transition deliberately stops the setup hotspot before attempting the
station join. AP+STA overlap is not a product dependency: on a single hosted
C6 it can reject a valid network when the hotspot and target access point use
incompatible channels. A failed clean join restores the same volatile hotspot
credential through the coordinator.

Phone/tablet acceptance passed on 2026-07-27. A real client joined the
OLED-advertised hotspot, received DHCP, and opened the captive setup page. A
deliberately incorrect station password failed within the bounded transition,
restored the same hotspot and password, and allowed the client to rejoin
without re-entering the hotspot credential. A valid home-network submission
then associated, received `192.168.1.237`, served HTTP on the LAN, and
published the working device-specific URL
`http://revlink-<identity-v1-id>.local`. This was the accepted identity-v1 hostname; the
current Nano field hostname is `http://revlink-<last-eight-mac>.local`.

The onboarding web surface keeps captive-network detection separate from the
normal product URL. Apple, Android, and Windows probe routes return their
platform-specific success response only after setup succeeds. Opening `/` or
`/index.html` on the device hostname then shows the embedded RevLink portal
placeholder instead of the probe response; `/setup` explicitly reopens the
connection assistant. The assistant includes a local Show/Hide control for
the station password, keeps connection progress at the top of the viewport,
and provides an in-page Done action because the native Apple captive-assistant
toolbar label is controlled by iPadOS/iOS.

The transfer boundary was accepted separately with a read-only 33-file
incremental sync. A forced network-mode change submitted immediately after
the sync entered `queued` was rejected, all 33 files reconciled as verified
cache hits, the AccessPort completed its acknowledged `0x05`/`0x35` close and
re-enumerated, and the network guard returned to idle. The guard begins at
`queued`, not only after the worker emits its first progress event, and
repeated same-state updates are intentionally idempotent.

This is still an acceptance runtime, not finished provisioning. Persistent
credential storage, factory reset, production authentication/CSRF policy, and
the remaining force-hotspot failure matrix remain open.
True encrypted NVS on this P4 requires a deliberate flash-encryption or
HMAC/eFuse provisioning decision and must not be enabled casually on a
development board.

## Required user experience

The production device should use a client-first, fallback-hotspot workflow:

1. At boot, look for saved Wi-Fi networks in explicit preference order.
2. If a preferred network is available, associate with it and obtain a local
   address. RevLink should then be reachable by a user connected to that same
   LAN.
3. If no saved network can be joined within a bounded startup period, start
   the device's own RevLink hotspot automatically.
4. If association or DHCP later fails for a sustained period, restore hotspot
   mode so the device does not become unreachable.
5. Factory reset must remove user-saved networks and return directly to the
   default hotspot/onboarding state.

Internet connectivity is not required to accept a client connection as
successful. Local association, a valid address, and a reachable RevLink
service are the important checks.

### Stale-link watchdog

On 2026-07-28 the P4 reproduced a split-brain failure: its coordinator and C6
snapshot both reported client-ready while inbound HTTP and the station data
path were unreachable. Disconnect callbacks alone cannot detect this state.

The runtime now performs one local-only ICMP probe to the DHCP gateway every
15 seconds while client-ready. Probes are paused during transfers. A healthy
reply clears the failure counter; three consecutive failures restart the
hosted radio stack and retry the same RAM-only network. The existing
30-second reconnect budget then restores the RevLink hotspot if needed.

If both station recovery and hotspot startup reach the faulted state, the
device performs one automatic reboot after 10 seconds. RTC-retained state
prevents a boot loop. The reboot allowance rearms only after 60 seconds of
stable client or hotspot operation.

The first-run hotspot page should make both product paths explicit:

- **Use RevLink directly** keeps the device hotspot active and opens the local
  product without requiring an internet uplink; and
- **Connect to Wi-Fi** joins a selected LAN or phone hotspot.

After a Wi-Fi submission, serve a transition page before taking the hotspot
down. It should say that RevLink is trying the selected network, explain that
the current browser connection may close, and show the device-specific
`.local` address to open after the join succeeds. A failed join restores the
same hotspot identity and credential so the user's phone or tablet can rejoin
automatically.

For the setup selector, perform one bounded scan immediately before starting
the fallback hotspot, retain only RAM-scoped deduplicated display results, and
offer the visible SSIDs as choices. Always retain a manual network-name field
for hidden networks. Do not repeatedly interrupt an active hotspot for scans
until AP-mode scanning has passed coexistence testing on the production radio
stack.

The current development priority policy is primary home Wi-Fi, secondary home
Wi-Fi, then the owner's phone hotspot. Selection happens only while
disconnected. Once any uplink succeeds, it remains sticky until that link
drops; the appearance of a higher-priority SSID must not interrupt it. A
60-second watchdog exits without scanning while connected and applies the
priority policy only after disconnection. The production event mechanism,
retry interval, and power cost must be reviewed rather than copied blindly.

## Default device identity

A new or reset device should advertise a predictable but unique SSID derived
from a stable factory Wi-Fi MAC address. Identity v2 uses:

```text
RevLink-<last eight hexadecimal MAC digits>
revlink-<last eight hexadecimal MAC digits>.local
```

Example:

```text
RevLink-A1B2C3D4
http://revlink-a1b2c3d4.local
```

The P4 implementation uses the stable factory AP MAC, not a randomized/private
MAC. It checks the bounded startup scan before committing its local identity.
If the visible SSID already exists, it tries `-2`, `-3`, and so on. The mDNS
responder performs its own standards-based conflict detection and may apply
the same kind of suffix if a conflict appears later. This is graceful
collision handling, not a claim that a truncated MAC can never collide.

Identity v2 separates three concepts that must not be conflated:

- the **Sidecar device ID**, generated once from 128 random bits and persisted
  as `r1_<32 lowercase hex digits>`;
- the **hardware MAC**, retained as local diagnostic/manufacturing metadata;
  and
- the **AccessPort storage namespace**, still derived from the attached
  AccessPort identity so incremental caches remain isolated across multiple
  AccessPorts.

The Sidecar ID is public identity, not a password or authentication token.
Ordinary firmware updates and restarts preserve it. Factory-reset semantics,
registration, ownership transfer, and secure manufacturing provisioning still
need a dedicated production acceptance plan.

The acceptance build uses an eight-character hardware-RNG-generated password
from a lowercase, ambiguity-free 31-symbol alphabet. It meets the WPA2 minimum,
avoids Shift and visually similar characters, provides about 40 bits of
randomness, appears only on the OLED, and is regenerated after every boot.

The production Wi-Fi credential should instead be a strong stable per-device
secret that survives ordinary restarts and changes only after an intentional
factory reset or credential rotation. It must not be derived solely from the
public MAC address. Before shipping, select its secure storage, recovery, and
manufacturing/provisioning flow. An open network or one shared factory
password is not acceptable for production.

## Discovery and multiple devices

The browser should advertise and display both the active network mode and the
address users should open:

- client mode: a unique mDNS hostname such as
  `revlink-a1b2c3d4.local`;
- hotspot mode: the same unique hostname plus a stable fallback gateway
  address; and
- transition state: a clear notice that the device is switching networks and
  which network the user should join.

A unique hostname is required because more than one RevLink may be present on
the same LAN.

The physical display should reserve a compact top-right Wi-Fi indicator.
Client mode should show the connected state and may scroll the active SSID
only when it does not obscure higher-priority USB, sync, or failure status.
Hotspot mode should continue to prioritize the setup SSID and credential.
The acceptance implementation now retains the generated setup credential only
in volatile runtime/display state after hiding it in client mode and restores
that same display state when the coordinator returns to hotspot mode.

The development-board physical shortcut uses **two rapid BOOT presses** to
show a full-screen standards-compatible Wi-Fi join QR while the fallback
hotspot is active. One further short press returns to the SSID/password view,
and the QR automatically expires after 30 seconds. The encoded password is
derived only from the volatile display credential, is never logged or
persisted, and the temporary text payload is zeroized after a display bitmap
is captured. This prototype gesture does not replace the future companion-app
and BLE provisioning path.

Live link-loss acceptance passed on 2026-07-27 using an iPhone hotspot. After
the hotspot was disabled, RevLink detected the station loss, attempted one
bounded reconnect to the same network, waited for the configured 30-second
reconnect budget, restored its own hotspot/DHCP service, and redisplayed the
same eight-character OLED credential. The OLED now projects the coordinator's
authoritative reconnect budget as `WIFI LOST`, a 30-to-0 seconds countdown,
progress bar, and `HOTSPOT NEXT`. Active sync and USB-safety states retain
higher display priority. When the budget expires, the existing hotspot
credential screen becomes authoritative automatically.

The iPhone joined successfully only with **Maximize Compatibility** enabled,
which exposes the required 2.4 GHz hotspot mode. Product onboarding must show
this instruction when an iPhone is selected or mentioned; it should not imply
that all phone-hotspot defaults are compatible.

## Trusted HTTPS identity

RevLink should treat discovery and trust as two separate concerns:

- `http://revlink-<suffix>.local` remains the zero-configuration discovery,
  captive-onboarding, and recovery address; and
- `https://revlink-<last8>.sidecar.revlinkgarage.com` becomes the stable
  trusted product origin after registration.

A publicly trusted certificate cannot be issued for a `.local` name. Shipping
a private certificate authority would replace the browser warning with a
device-management burden and is not an acceptable consumer setup flow.
The `.local` endpoint therefore remains intentionally local HTTP and must
never expose a privileged unauthenticated operation.

Each production Sidecar must own a unique TLS private key and certificate.
Never ship one wildcard private key shared by every device. The per-device key
should be generated or injected during a controlled provisioning step, stored
in hardware-backed or encrypted storage selected for the final board, and
rotatable without erasing user data. The public device identity must bind the
certificate name, Wi-Fi identity, BLE identity, and application pairing
record to the same factory device identifier.

The trusted hostname needs deterministic local resolution in both network
modes:

- in hotspot mode, the Sidecar's captive DNS resolves only its own trusted
  hostname to the local gateway address;
- in client mode, the device or companion service updates a short-lived DNS
  record to the current private LAN address; and
- if LAN DNS is unavailable or stale, mDNS and BLE remain recovery paths that
  can rediscover the current address without weakening the TLS identity.

This split also creates the production upgrade path for the native companion
application. The app may pin the paired device identity and open the same
HTTPS origin used by the browser while BLE handles discovery and recovery.
Pairing, certificate renewal, clock validity, private-key recovery, and
offline-first behavior must be designed before enabling HTTPS writes.

## Physical recovery gesture candidate

Reserve a candidate gesture of **five rapid BOOT presses** to request a
bounded scan and connection attempt against the preferred saved-network list.
Do not bind the gesture until the complete physical-control map is reviewed;
the same button already owns the prototype double-press Wi-Fi QR and
long-hold shutdown actions, and the product also needs safe eject,
reset/recovery, factory reset, and user-feedback semantics.

If adopted, the gesture must emit the same authenticated
`RETRY_SAVED_NETWORKS` product command used by HTTP/BLE rather than calling
the radio adapter directly. It must be ignored while a transfer or other
non-interruptible operation holds the safety lock, use a finite multi-press
window, display acknowledgment, and preserve automatic hotspot recovery if no
preferred network is available.

## BLE companion and recovery

BLE is a discovery and control plane, not a file-transfer transport. A future
iOS/Android companion application should use it to:

- discover and identify a nearby RevLink;
- provision or repair Wi-Fi credentials;
- report current network mode, address, AccessPort state, storage, battery,
  synchronization progress, and actionable errors;
- request a bounded saved-network retry or force the RevLink hotspot; and
- open the local Wi-Fi web interface after connectivity is established.

Datalogs, maps, startup images, updates, and other bulk data remain on Wi-Fi.
The browser interface remains the complete product interface; the companion
application is a branded onboarding and recovery assistant rather than a
second implementation of every feature.

BLE must be implemented as a replaceable authenticated adapter that emits the
same product commands as HTTP and physical controls. It must not call the
radio, USB, or storage drivers directly. BLE support and Wi-Fi/BLE coexistence
on the P4/C6 path remain unaccepted hardware work. Until the mobile
application exists, the fallback hotspot and local onboarding page are the
no-app recovery path.

## Architecture review items

Before choosing the final radio/module and network stack, validate:

- whether simultaneous station and access-point mode is reliable on the
  selected hardware;
- whether the hotspot should remain active while connected to a LAN or be
  enabled only as a fallback;
- the scan, connection, DHCP, and fallback time budgets;
- saved-network priority, retry backoff, and handling of wrong passwords;
- transition behavior when a preferred network appears while clients are
  using the fallback hotspot;
- 2.4 GHz and 5 GHz support, regulatory configuration, and phone-hotspot
  compatibility;
- captive-portal onboarding and recovery without a display;
- secure credential storage, reset behavior, and credential rotation;
- mDNS behavior across iOS, Android, macOS, and Windows;
- per-device TLS key provisioning, renewal, revocation, and factory-reset
  behavior;
- split-horizon/local DNS behavior for the trusted
  `*.sidecar.revlinkgarage.com` origin in hotspot and LAN modes;
- browser behavior when a new device has no trusted wall clock yet;
- OTA/update behavior while operating without internet access; and
- tests proving the device remains recoverable after power loss during every
  network transition;
- BLE authentication, bonding, recovery authorization, and credential
  rotation;
- Wi-Fi throughput and latency while BLE remains discoverable; and
- App Store and Google Play companion-app release work during later
  custom-board or manufacturing downtime.

## Acceptance scenarios

The final implementation should pass at least these cold-boot tests:

1. Preferred home Wi-Fi present: join it, publish the unique hostname, and
   serve RevLink to another client on that LAN.
2. Preferred phone hotspot present: join it and remain usable through normal
   hotspot address changes.
3. No saved Wi-Fi present: start the unique RevLink hotspot automatically.
4. Saved Wi-Fi has a wrong password: time out and restore hotspot access.
5. Preferred Wi-Fi disappears after connection: recover to hotspot mode.
6. Preferred Wi-Fi returns: follow the chosen migration policy without a
   reboot loop or unreachable state.
7. Factory reset: erase saved network credentials and restore the default
   unique hotspot identity.
8. Two RevLink devices on one LAN: both remain independently discoverable.
9. Provisioned HTTPS device on its own hotspot: the trusted hostname resolves
   locally, presents that device's certificate, and never uses another
   Sidecar's private key.
10. Provisioned HTTPS device moving between hotspot and LAN modes: the
    hostname follows the device without a certificate-name change, and stale
    DNS recovers through mDNS or BLE.
