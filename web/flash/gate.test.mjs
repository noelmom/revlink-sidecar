import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createHash } from "node:crypto";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

import { evaluateGate, parseRevision } from "./gate.js";

const here = dirname(fileURLToPath(import.meta.url));

/*
 * Follow the same pointer the page follows, rather than naming a release here.
 * A hard-coded copy once left these tests checking a release the page no
 * longer served, which is the one failure mode a release test must not have.
 */
const releases = JSON.parse(
  readFileSync(join(here, "releases.json"), "utf8")
);
if (!releases.current) {
  throw new Error("releases.json names no current release");
}
const RELEASE = join(here, "firmware", releases.current);
const manifest = JSON.parse(
  readFileSync(join(RELEASE, "manifest.json"), "utf8")
);

test("Nano silicon is accepted", () => {
  const verdict = evaluateGate(manifest, {
    description: "ESP32-P4 (revision v1.0)",
    major: 1,
  });
  assert.equal(verdict.ok, true);
  assert.equal(verdict.reason, "match");
});

test("Dev Kit silicon is refused, not merely warned about", () => {
  const verdict = evaluateGate(manifest, {
    description: "ESP32-P4 (revision v3.0)",
    major: 3,
  });
  assert.equal(verdict.ok, false);
  assert.equal(verdict.reason, "incompatible-silicon");
  assert.match(verdict.message, /v3\.x/);
  assert.match(verdict.message, /NANO/i);
});

test("an unreadable revision refuses rather than guessing", () => {
  for (const major of [null, undefined, Number.NaN]) {
    const verdict = evaluateGate(manifest, {
      description: "ESP32-P4",
      major,
    });
    assert.equal(verdict.ok, false, `major=${String(major)} must refuse`);
    assert.equal(verdict.reason, "unknown-revision");
  }
});

test("a different chip family is refused", () => {
  for (const chip of ["ESP32-S3 (revision v0.2)", "ESP32-C6", "ESP8266", ""]) {
    const verdict = evaluateGate(manifest, { description: chip, major: 1 });
    assert.equal(verdict.ok, false, `${chip || "(empty)"} must refuse`);
    assert.equal(verdict.reason, "wrong-chip");
  }
});

test("ESP32-P4 is not matched loosely by another P4-ish name", () => {
  // Guards against a substring match letting an unrelated chip through.
  const verdict = evaluateGate(manifest, {
    description: "ESP32-C3 (revision v0.4)",
    major: 0,
  });
  assert.equal(verdict.ok, false);
  assert.equal(verdict.reason, "wrong-chip");
});

test("revision parsing handles the shapes esptool reports", () => {
  assert.deepEqual(parseRevision("ESP32-P4 (revision v1.0)"), {
    major: 1,
    minor: 0,
  });
  assert.deepEqual(parseRevision("ESP32-P4 (revision v3.12)"), {
    major: 3,
    minor: 12,
  });
  assert.deepEqual(parseRevision("ESP32-P4"), { major: null, minor: null });
  assert.deepEqual(parseRevision(undefined), { major: null, minor: null });
});

test("every manifest part exists and matches its recorded digest", () => {
  assert.ok(manifest.parts.length > 0, "manifest lists no parts");
  const offsets = new Set();
  for (const part of manifest.parts) {
    const bytes = readFileSync(join(RELEASE, part.path));
    assert.equal(
      bytes.length,
      part.size,
      `${part.path}: size does not match the manifest`
    );
    assert.equal(
      createHash("sha256").update(bytes).digest("hex"),
      part.sha256,
      `${part.path}: SHA-256 does not match the manifest`
    );
    assert.equal(
      offsets.has(part.offset),
      false,
      `${part.path}: duplicate flash offset`
    );
    offsets.add(part.offset);
  }
});

test("parts do not overlap in flash", () => {
  const sorted = [...manifest.parts].sort((a, b) => a.offset - b.offset);
  for (let i = 1; i < sorted.length; i += 1) {
    const previous = sorted[i - 1];
    const end = previous.offset + previous.size;
    assert.ok(
      end <= sorted[i].offset,
      `${previous.path} ends at 0x${end.toString(16)} which overruns ` +
        `${sorted[i].path} at 0x${sorted[i].offset.toString(16)}`
    );
  }
});

test("the manifest version matches the release directory it lives in", () => {
  // A version string has to name one set of bytes. If a directory called
  // v0.2.1-nano can contain a manifest saying 0.2.0, the version means
  // nothing and a bug report against it cannot be resolved.
  const expected = releases.current.replace(/^v/, "").replace(/-.*$/, "");
  assert.equal(
    manifest.version,
    expected,
    `manifest says ${manifest.version} but lives in ${releases.current}`
  );
});

test("every release named in releases.json exists and verifies", () => {
  for (const entry of releases.releases ?? []) {
    const dir = join(here, "firmware", entry.id);
    const m = JSON.parse(readFileSync(join(dir, "manifest.json"), "utf8"));
    assert.equal(m.version, entry.version, `${entry.id}: version disagrees`);
    for (const part of m.parts) {
      const bytes = readFileSync(join(dir, part.path));
      assert.equal(
        createHash("sha256").update(bytes).digest("hex"),
        part.sha256,
        `${entry.id}/${part.path}: digest mismatch`
      );
    }
  }
});

test("the published image matches what the manifest claims about writes", () => {
  const app = readFileSync(join(RELEASE, "revlink-sidecar.bin"));
  const probes = [
    "Map-write capability compiled",
    "Runtime map-write consent",
  ];
  const present = probes.filter((s) =>
    app.includes(Buffer.from(s, "latin1"))
  );

  if (manifest.deviceWrites) {
    assert.deepEqual(
      present,
      probes,
      "manifest says writes are compiled in, but the write paths are absent"
    );
  } else {
    assert.deepEqual(
      present,
      [],
      "manifest says writes are compiled out, but the image contains them"
    );
  }
});

test("a write-capable image still ships with owner consent locked", () => {
  if (!manifest.deviceWrites) return;
  const app = readFileSync(join(RELEASE, "revlink-sidecar.bin"));
  // The startup banner is emitted unconditionally when the capability is
  // compiled in, and states the runtime default. Its wording is the contract.
  assert.ok(
    app.includes(Buffer.from("runtime consent is OFF", "latin1")),
    "write-capable image does not declare consent locked at startup"
  );
});
