/*
 * Board-compatibility gate for the web flasher.
 *
 * The ESP32-P4-NANO and the ESP32-P4-WIFI6-DEV-KIT both report as "ESP32-P4"
 * but use pre-v3 and v3+ silicon, which ESP-IDF treats as mutually
 * incompatible firmware targets. Flashing the wrong image leaves a board that
 * will not boot, so this refuses before a single byte is written rather than
 * relying on the bootloader to reject the image afterwards.
 *
 * Kept in its own module with no browser dependencies so it can be tested
 * without a board and without a browser. See gate.test.mjs.
 */

/** Pull a major/minor silicon revision out of an esptool chip description. */
export function parseRevision(description) {
  const match = /v(\d+)\.(\d+)/i.exec(String(description ?? ""));
  if (!match) return { major: null, minor: null };
  return { major: Number(match[1]), minor: Number(match[2]) };
}

/**
 * @returns {{ok: boolean, reason: string, message: string}}
 * `ok` is true only when the connected board matches the manifest. Any
 * uncertainty — an unreadable revision, an unexpected chip — is a refusal.
 */
export function evaluateGate(manifest, { description, major }) {
  const normalise = (s) => String(s ?? "").replace(/[^a-z0-9]/gi, "").toLowerCase();
  const family = normalise(manifest.chipFamily);
  const seen = normalise(description);

  if (!seen.includes(family)) {
    return {
      ok: false,
      reason: "wrong-chip",
      message:
        `This build is for ${manifest.chipFamily}, but the connected board ` +
        `reports "${description || "an unknown chip"}". Nothing was written.`,
    };
  }

  if (major === null || major === undefined || Number.isNaN(major)) {
    return {
      ok: false,
      reason: "unknown-revision",
      message:
        "The silicon revision could not be read, so the board cannot be " +
        "matched against this build. Nothing was written.",
    };
  }

  if (major >= manifest.silicon.maxMajorRevisionExclusive) {
    return {
      ok: false,
      reason: "incompatible-silicon",
      message: manifest.silicon.rejectMessage.replace("{rev}", `${major}.x`),
    };
  }

  return { ok: true, reason: "match", message: "" };
}
