# Vehicle Health

RevLink Vehicle Health is a deterministic, local-first evidence layer built
from the immutable datalogs already cached for an AccessPort. It is not a
parts-replacement oracle, an ECU-flashing feature, or a substitute for a
qualified diagnosis.

## Product model

Vehicle Health and focused Investigations are separate workflows:

- **Vehicle Health** reviews the newest 5, 10, 15, 20, 25, or 30 datalogs and
  shows how the available evidence changes over time.
- **Investigations** preserve the symptom-led workflow for a trouble code,
  driver observations, selected logs, and a verification plan.
- A future, separately entitled AI layer may explain the deterministic
  evidence and help users choose relevant logs. It must not silently change the
  underlying score or evidence.

The health timeline is organized around immutable evidence:

```text
device namespace
  -> log SHA-256 + initial-sync timestamp
    -> engine version + deterministic result
      -> exact map hash, unambiguous declared map, or unresolved context
```

The currently selected point exposes the source log, score, confidence,
detected operating conditions, threshold signals, and map provenance. Scrubbing
the chart never mutates a log or map.

## Score and confidence

Health and confidence are intentionally independent.

- **Health** starts at 100 and is reduced only when a conservative,
  documented rule crosses a threshold.
- **Confidence** describes channel coverage, sample count, and whether useful
  idle, cruise, or load conditions were present.
- Missing channels lower confidence. They do not lower health.
- A high score with low confidence means "no threshold crossed in limited
  evidence," not "the vehicle is proven healthy."

Version 1.0 evaluates one CSV at a time and currently recognizes:

- dynamic advance multiplier;
- feedback and learned knock correction;
- fuel correction and learning magnitude;
- coolant and intake temperature;
- charging voltage while engine speed is present;
- cylinder roughness;
- stationary low-throttle RPM variation and near-stall behavior; and
- coarse idle, cruise, and load condition coverage.

Thresholds are deliberately conservative and must remain versioned. Changing a
rule requires a new engine version so cached results are never confused with
results from another ruleset.

## Map provenance

Map context is resolved in this order:

1. an exact user-saved datalog-to-map tag, stored as the immutable cached map
   SHA-256;
2. the map name declared in the datalog's `AP Info` header, but only when it
   has one unambiguous match in that device's cached maps; or
3. unresolved context, shown explicitly.

Filename-only matches must never override an exact hash. A reused filename or
multiple cached versions stays ambiguous until the user selects the correct
map.

The `.ptm` reader uses only the header fields a map file declares about
itself in the clear: description, declared vehicle compatibility,
vendor/version, identifiers, checksums, and feature declarations. That is
enough to tell you which map a datalog came from, which is what Vehicle Health
needs.

It stops there deliberately. **RevLink Sidecar does not decode protected or
vendor-locked map content.** Tuners protect maps on purpose — it is how many
of them make a living — and reading the metadata a file publishes about itself
is a different thing from defeating that protection.

A numeric "tuning delta" against a stock baseline is therefore out of scope.
It would require exactly the decoding this project declines to do, and it
would mean reading calibration data off the vehicle. Neither is a goal.

## Resource model

The ESP32-P4 remains well within its hardware envelope:

- the latest full production-profile image is 1,372,528 bytes in a 4 MiB
  application partition, leaving 67% free;
- the board has 32 MiB PSRAM; and
- immutable source files live on the Sidecar microSD card; the engine does not
  depend on a specific card capacity.

Vehicle Health evaluates one log at a time and caches a compact derived result
by device namespace, log SHA-256, and engine version. The current prototype
uses browser-local derived-result caching; the source of truth remains the
microSD objects and annotations. A future device-wide cache may persist the
same versioned result schema on microSD.

Heavy operations stay modular:

- CSV evaluation can run locally in the browser;
- PDF rendering remains an export adapter, not part of the scoring engine; and
- AI consumes deterministic evidence but cannot replace it.

This prevents optional PDF, cloud, or AI features from consuming
always-resident firmware memory or blocking USB synchronization.

## Safety and acceptance

- Vehicle Health is read-only.
- It never sends ECU commands or changes AccessPort storage.
- Evidence always remains attributable to the exact log hash and engine
  version.
- The UI must disclose low confidence and unresolved map context.
- Threshold tests use synthetic fixtures plus representative captured CSVs.
- Before beta, validate mixed Subaru and donor-device logs, missing-channel
  behavior, filename rotation, exact map tags, ambiguous map names, and
  30-log bounded analysis on the ESP32-P4 portal.
