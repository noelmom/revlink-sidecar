import { ESPLoader, Transport } from "./vendor/esptool-js-0.6.1.js";
import { evaluateGate, parseRevision } from "./gate.js";

const RELEASE = "firmware/v0.1.0-nano-readonly";

const el = (id) => document.getElementById(id);
const ui = {
  support: el("support"),
  connect: el("connect"),
  flash: el("flash"),
  disconnect: el("disconnect"),
  device: el("device"),
  deviceBody: el("device-body"),
  status: el("status"),
  progress: el("progress"),
  bar: el("bar"),
  log: el("log"),
  release: el("release"),
};

let manifest = null;
let transport = null;
let esploader = null;
let gateOk = false;

function log(line) {
  ui.log.textContent += line + "\n";
  ui.log.scrollTop = ui.log.scrollHeight;
}

function status(message, kind = "") {
  ui.status.textContent = message;
  ui.status.className = "status" + (kind ? " " + kind : "");
}

// esptool-js writes progress and chip banter through a terminal-ish object.
const terminal = {
  clean() {},
  writeLine(data) { log(String(data)); },
  write(data) { ui.log.textContent += String(data); },
};

async function loadManifest() {
  const response = await fetch(`${RELEASE}/manifest.json`, { cache: "no-store" });
  if (!response.ok) throw new Error(`manifest ${response.status}`);
  manifest = await response.json();
  ui.release.textContent =
    `${manifest.name} ${manifest.version} · ${manifest.profile} · ` +
    `ESP-IDF ${manifest.idfVersion} · built ${manifest.buildDate}`;
}

function checkSupport() {
  if (!("serial" in navigator)) {
    ui.support.hidden = false;
    ui.connect.disabled = true;
    return false;
  }
  if (!window.isSecureContext) {
    status(
      "This page must be served over HTTPS (or from localhost) for Web Serial to work.",
      "error"
    );
    ui.connect.disabled = true;
    return false;
  }
  return true;
}

async function sha256Hex(buffer) {
  const digest = await crypto.subtle.digest("SHA-256", buffer);
  return [...new Uint8Array(digest)]
    .map((b) => b.toString(16).padStart(2, "0"))
    .join("");
}

/*
 * Fetch every part and verify it against the digest recorded in the manifest
 * before a single byte reaches the board. A truncated download or a corrupted
 * mirror should fail here, not halfway through writing flash.
 */
async function fetchParts() {
  const parts = [];
  for (const part of manifest.parts) {
    status(`Downloading ${part.path}…`);
    const response = await fetch(`${RELEASE}/${part.path}`, { cache: "no-store" });
    if (!response.ok) throw new Error(`${part.path}: HTTP ${response.status}`);
    const buffer = await response.arrayBuffer();

    if (buffer.byteLength !== part.size) {
      throw new Error(
        `${part.path}: expected ${part.size} bytes, got ${buffer.byteLength}`
      );
    }
    const actual = await sha256Hex(buffer);
    if (actual !== part.sha256) {
      throw new Error(
        `${part.path}: SHA-256 mismatch.\n  expected ${part.sha256}\n  actual   ${actual}`
      );
    }
    log(`verified ${part.path} (${part.size} bytes)`);

    // esptool-js takes a latin1 "binary string", not an ArrayBuffer.
    const bytes = new Uint8Array(buffer);
    let binary = "";
    for (let i = 0; i < bytes.length; i += 0x8000) {
      binary += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
    }
    parts.push({ data: binary, address: part.offset });
  }
  return parts;
}

/*
 * The Nano and the Dev Kit are both "ESP32-P4" but use pre-v3 and v3+ silicon,
 * which ESP-IDF treats as mutually incompatible targets. Flashing the wrong one
 * leaves a board that will not boot, so refuse before writing rather than
 * letting the bootloader reject the image afterwards.
 */
async function readSilicon() {
  let major = null;
  let minor = null;
  try {
    major = await esploader.chip.getMajorChipVersion(esploader);
    minor = await esploader.chip.getMinorChipVersion(esploader);
  } catch {
    /* fall through to parsing the description */
  }

  let description = "";
  try {
    description = await esploader.chip.getChipDescription(esploader);
  } catch {
    description = esploader.chip?.CHIP_NAME ?? "unknown";
  }

  if (major === null) {
    ({ major, minor } = parseRevision(description));
  }
  return { description, major, minor };
}

function renderDevice({ description, major, minor }, verdict) {
  const rev = major === null ? "unknown" : `v${major}.${minor ?? 0}`;
  ui.deviceBody.innerHTML = "";
  const rows = [
    ["Chip", description || "unknown"],
    ["Silicon revision", rev],
    ["Target of this build", manifest.silicon.targetBoard],
    ["Device writes", manifest.deviceWrites ? "compiled in" : "compiled out"],
  ];
  for (const [k, v] of rows) {
    const dt = document.createElement("dt");
    dt.textContent = k;
    const dd = document.createElement("dd");
    dd.textContent = v;
    ui.deviceBody.append(dt, dd);
  }
  ui.device.hidden = false;
  ui.device.classList.toggle("blocked", !verdict.ok);
}

async function connect() {
  ui.connect.disabled = true;
  ui.log.textContent = "";
  try {
    status("Select the Sidecar's serial port…");
    const port = await navigator.serial.requestPort();
    transport = new Transport(port, true);
    esploader = new ESPLoader({
      transport,
      baudrate: 460800,
      romBaudrate: 115200,
      terminal,
      enableTracing: false,
    });

    status("Connecting…");
    await esploader.main();

    const silicon = await readSilicon();
    const verdict = evaluateGate(manifest, silicon);
    renderDevice(silicon, verdict);

    if (!verdict.ok) {
      gateOk = false;
      ui.flash.disabled = true;
      status(verdict.message, "error");
    } else {
      gateOk = true;
      ui.flash.disabled = false;
      status("Board matches this build. Ready to flash.", "ok");
    }
    ui.disconnect.disabled = false;
  } catch (error) {
    status(describe(error), "error");
    log(String(error?.stack || error));
    await disconnect();
  } finally {
    if (!esploader) ui.connect.disabled = false;
  }
}

function describe(error) {
  const message = String(error?.message || error);
  if (/No port selected/i.test(message)) {
    return "No port selected. Click Connect and choose the Sidecar's port.";
  }
  if (/Failed to open serial port|access denied/i.test(message)) {
    return (
      "Could not open the serial port. Close any other program using it " +
      "(a serial monitor, idf.py monitor, Arduino IDE) and try again."
    );
  }
  if (/Timed out waiting for packet header|Failed to connect/i.test(message)) {
    return (
      "The board did not answer. Hold BOOT, tap RST, release BOOT to enter " +
      "download mode, then Connect again."
    );
  }
  return message;
}

async function flash() {
  if (!gateOk) return;
  ui.flash.disabled = true;
  ui.connect.disabled = true;
  ui.disconnect.disabled = true;
  ui.progress.hidden = false;

  try {
    const fileArray = await fetchParts();
    const total = fileArray.reduce((n, f) => n + f.data.length, 0);
    const done = new Array(fileArray.length).fill(0);

    status("Writing flash — do not unplug the board.");
    await esploader.writeFlash({
      fileArray,
      flashSize: manifest.flash.size,
      flashMode: manifest.flash.mode,
      flashFreq: manifest.flash.frequency,
      eraseAll: false,
      compress: true,
      reportProgress: (index, written) => {
        done[index] = written;
        const sum = done.reduce((a, b) => a + b, 0);
        const pct = Math.min(100, Math.round((sum / total) * 100));
        ui.bar.style.width = pct + "%";
        ui.bar.textContent = pct + "%";
      },
    });

    await esploader.after();
    status(
      "Done. The Sidecar has restarted — look for its Wi-Fi network, or its " +
      "OLED if one is fitted.",
      "ok"
    );
  } catch (error) {
    status(describe(error), "error");
    log(String(error?.stack || error));
  } finally {
    ui.disconnect.disabled = false;
  }
}

async function disconnect() {
  try {
    await transport?.disconnect();
  } catch {
    /* the port may already be gone */
  }
  transport = null;
  esploader = null;
  gateOk = false;
  ui.connect.disabled = false;
  ui.flash.disabled = true;
  ui.disconnect.disabled = true;
  ui.device.hidden = true;
  ui.progress.hidden = true;
  ui.bar.style.width = "0%";
  ui.bar.textContent = "";
}

ui.connect.addEventListener("click", connect);
ui.flash.addEventListener("click", flash);
ui.disconnect.addEventListener("click", disconnect);

loadManifest()
  .then(() => {
    if (checkSupport()) status("Connect the Sidecar over USB-C, then click Connect.");
  })
  .catch((error) => {
    status(`Could not load the release manifest: ${error.message}`, "error");
    ui.connect.disabled = true;
  });
