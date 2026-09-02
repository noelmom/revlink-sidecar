# Single-AccessPort safety gate

Status: **software guard implemented; idle powered-hub conflict and deliberate
recovery accepted; the remaining powered-hub matrix is still required before
beta**.

The shipping RevLink Sidecar has one downstream USB connector, but a customer
can still attach a USB hub and connect more than one AccessPort. Physical
connector count is therefore not a sufficient safety control. RevLink must
enforce exactly one eligible AccessPort in software before it starts any
identity, file, write, delete, or recovery transaction.

The ESP32-P4 adapter now counts only live devices that satisfy the complete
high-speed AccessPort endpoint contract. It refuses every transaction unless
that count is exactly one, pins accepted work to the USB handle and attachment
generation, and latches a first-class conflict when another eligible unit
appears. The application independently blocks device actions, cancels an
active read-only sync cooperatively, disarms automatic sync after recovery,
and exposes the fault in the portal and OLED. The transport also assigns every
eligible-device topology change a monotonic revision. Both the application and
core reject a late event whose revision predates the current topology, so a
stale detach cannot clear a newer conflict.

The production image and platform-neutral lifecycle tests pass. On 2026-07-29,
the idle attach and deliberate-recovery slice of the powered-hub matrix passed
with two physical AccessPorts. Do not describe the complete beta gate as
accepted until the remaining hardware evidence is preserved.

## Product rule

The transport derives one of three mutually exclusive topology states:

| Eligible AccessPorts | State | Allowed behavior |
| --- | --- | --- |
| 0 | `none` | Offline browsing only |
| 1 | `single` | Normal bounded operations |
| 2 or more | `conflict` | No device operation may start |

An eligible AccessPort is a live USB device whose standard descriptors match
the supported VID/PID and required high-speed endpoint contract. The guard
must run before the proprietary identity handshake. It must not depend on the
USB serial string, USB address, cached vehicle, filename, or previously
selected dataset.

Unrelated USB devices do not count as AccessPorts and must never receive
AccessPort protocol traffic. Their presence may be reported for diagnostics,
but it must not make RevLink select a different AccessPort.

## Conflict behavior

When two or more eligible AccessPorts are present, RevLink must:

- enter a first-class `multiple-devices`/`conflict` lifecycle state;
- reject manual sync, auto-sync, close recovery, map upload, startup-screen
  apply, deletion, and every future device-management action;
- expose no device picker in the consumer UI;
- show **Multiple AccessPorts detected — unplug all devices, then reconnect
  one** in the portal and on the physical display;
- preserve cached data for offline browsing without assigning either physical
  device to a cached namespace; and
- never retry, switch targets, or choose the lowest USB address.

The application authorization gate and the USB adapter must both reject the
operation. A browser-only restriction is insufficient.

## A second device during an active operation

Every transaction is pinned to the exact USB handle and attachment generation
selected at its start. A newly enumerated device can never replace that target.

For a read-only sync, detecting a second AccessPort requests cooperative
cancellation at the next safe boundary, performs the accepted close sequence
against only the pinned device when possible, and then enters `conflict`.

Writes and deletes ship as of 0.2.2, and what protects them today is the
transport, not the absence of the feature. Every USB transaction — read, write
or delete — is refused unless exactly one eligible AccessPort is enumerated and
no conflict is latched (`revlink_accessport_usb.c`, the
`conflict_latched || eligible_accessport_count(state) != 1U` guards). A second
device arriving therefore stops the next transaction rather than retargeting
it, and detection never retries or redirects a write.

What is **not** yet proven is the harder case: a second AccessPort arriving
*during* a write, rather than between transactions. The intended policy is that
the write may finish only the already-defined non-interruptible protocol step
needed to leave the original target in a known state, then close or fault and
enter `conflict`. That path has not been exercised on hardware.

## Recovery after the customer unplugs one

Conflict recovery must be deliberate:

1. Observe that only one eligible AccessPort remains for the full detach
   debounce interval.
2. Clear stale queued requests, cached physical selection, and auto-sync
   attachment latches.
3. Keep automatic operations disarmed.
4. Ask the customer to confirm **Continue with the remaining AccessPort**, or
   require a complete zero-device-to-one-device physical reattachment.
5. Re-enumerate and read the remaining device's true identity before selecting
   its cache namespace.

RevLink must not immediately continue an operation merely because the count
fell from two to one.

## Polite re-enumeration

The accepted `0x05`/`0x35` close causes the same AccessPort to detach and
re-enumerate. That expected lifecycle must not produce a false multiple-device
alarm.

The adapter must distinguish:

- a handle already marked gone/closing followed by its expected bounded
  software re-enumeration; from
- two simultaneously live, accepted physical handles.

An expected re-enumeration window alone is not permission to ignore a second
live device. Only a predecessor already in the closing/gone lifecycle may be
excluded from the live candidate count.

## Required host tests

- zero eligible devices rejects device operations;
- exactly one eligible device is selected;
- two eligible devices present before a request enter `conflict`;
- devices enumerated in either address/order produce the same refusal;
- a second eligible device arriving while idle enters `conflict`;
- a second device arriving during read-only sync pins the original target,
  requests safe cancellation, and never opens a session on the newcomer;
- conflict blocks auto-sync and close recovery as well as manual sync;
- removal of one device does not immediately resume queued work;
- a stable remaining device requires explicit recovery or full reattachment;
- an unrelated USB device plus one AccessPort does not change the target;
- a normal acknowledged software re-enumeration does not create a false
  conflict; and
- stale events from a prior attachment generation cannot clear a newer
  conflict.

The reversed address/enumeration-order and stale topology revision cases pass
in the platform-neutral host suite. The remaining unchecked host-matrix cases
and the physical powered-hub matrix remain part of the beta gate.

Write-specific host tests remain outstanding: map upload, startup-screen
apply, overwrite, delete, cancellation, and transport failure should each
prove that a second device cannot change the pinned target. The capability
shipped before these were written, on the strength of the transport-level
guard above; they are still worth having, because that guard is one condition
in two places rather than a tested invariant.

## Required hardware acceptance

Use a known-good powered high-speed hub and two supported AccessPorts:

1. Boot with both attached and confirm neither enters a RevLink session.
2. Attach the second unit while the first is idle.
3. Attach the second unit during a read-only sync.
4. Repeat with the hub ports and enumeration order reversed.
5. Unplug each unit in turn and verify deliberate recovery.
6. Run a normal single-device sync and accepted close/re-enumeration to prove
   there is no false conflict.
7. Attach one AccessPort plus an unrelated USB device and confirm that only
   the AccessPort is eligible.
8. Power-cycle the Sidecar in the conflict topology and confirm the default is
   still fail-closed.

Record both true AccessPort identities, USB topology, portal/OLED state,
transaction logs, cancellation/close result, and proof that no request was
sent to the second unit.

The read-only matrix and the host suite are what this gate was written
against. Write acceptance under conflict — a second device arriving mid-write —
remains open and is the main thing this document still asks for.

## Live hardware evidence

### 2026-07-29: idle attach and deliberate recovery — passed

A powered high-speed hub and two physical AccessPorts were tested against the
ESP32-P4 development Sidecar:

- baseline discovery reported exactly one eligible `AP3-VLK-002`, high-speed
  USB, 512-byte packets, no active transfer, and auto-sync disabled;
- attaching the second AccessPort changed `eligibleCount` from 1 to 2, entered
  `multiple-devices`, cleared device availability, set
  `conflictRecoveryRequired`, and left sync inactive;
- the portal exposed the required conflict and full-detach guidance;
- removing only one AccessPort changed the count to 1 but deliberately kept
  the conflict latch set;
- removing both changed the state to `waiting`, cleared the conflict latch,
  and did not start any automatic operation; and
- reconnecting one AccessPort restored the supported identity, high-speed
  endpoint contract, and normal `available` state without rebooting.

Writes and deletes were compiled out *in the image used for this run*; both
ship today. No sync or device mutation was attempted,
and no USB packet trace was captured during this slice. The remaining matrix
items—boot with both attached, conflict during read-only sync, reversed hub
ports/enumeration order, unrelated USB hardware, accepted close/re-enumeration,
and conflict-topology power cycle—remain open.
