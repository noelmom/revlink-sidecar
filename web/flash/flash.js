import {
  ESPLoader,
  Transport,
  HardReset,
  ClassicReset,
  UsbJtagSerialReset,
} from "./vendor/esptool-js-0.6.1.js";
import { evaluateGate, parseRevision } from "./gate.js";

/*
 * Which build to serve is data, not code. Release directories are immutable
 * once published — a version string has to name one set of bytes, or a bug
 * report against it means nothing — so publishing adds a directory and moves
 * this pointer rather than overwriting anything.
 */
const RELEASES = "releases.json";
let RELEASE = null;

/*
 * Transfer speed. A USB-serial bridge that syncs happily at 460800 can
 * still drop bytes once a sustained megabyte is moving through it, which
 * shows up as a write that dies partway with the board still attached.
 * Default to the conservative rate and let the impatient opt up.
 */
const selectedBaud = () =>
  Number(document.getElementById("baud")?.value) || 115200;

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
  baud: el("baud"),
  erase: el("erase"),
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
  const index = await fetch(RELEASES, { cache: "no-store" });
  if (!index.ok) throw new Error(`releases ${index.status}`);
  const { current } = await index.json();
  if (!current) throw new Error("no current release is named");
  RELEASE = `firmware/${current}`;

  const response = await fetch(`${RELEASE}/manifest.json`, { cache: "no-store" });
  if (!response.ok) throw new Error(`manifest ${response.status}`);
  manifest = await response.json();
  ui.release.textContent =
    `${manifest.name} ${manifest.version} · ${manifest.profile} · ` +
    (manifest.commit ? `commit ${manifest.commit} · ` : "") +
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

    /*
     * esptool-js takes a Uint8Array. Handing it a latin1 "binary string" —
     * the pre-0.5 convention — makes the deflate step encode it as UTF-8, so
     * every byte above 0x7F becomes two and the device is sent a stream
     * larger than the image. That fails at the same block on every attempt,
     * at any baud rate, which looks convincingly like a flaky serial link.
     */
    parts.push({ data: new Uint8Array(buffer), address: part.offset });
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
    transport = new Transport(port, false);
    esploader = new ESPLoader({
      transport,
      baudrate: selectedBaud(),
      romBaudrate: 115200,
      terminal,
      enableTracing: false,
      /*
       * Without these, after("hard_reset") finds no constructor and returns
       * having done nothing — silently, with no error. That is why the board
       * sat in the bootloader after a successful write and had to be power
       * cycled by hand, on every operating system.
       */
      resetConstructors: {
        hardReset: (t, usingUsbOtg) => new HardReset(t, usingUsbOtg),
        classicReset: (t, delay) => new ClassicReset(t, delay),
        usbJTAGSerialReset: (t) => new UsbJtagSerialReset(t),
      },
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
  if (/Failed to write compressed data|Failed to write to target Flash/i.test(
        message
      )) {
    return (
      "The write stopped partway. Your board is almost certainly fine — a " +
      "half-written board is not a broken one, because the bootloader lives " +
      "in ROM. Press Flash to try again; you do not need to reconnect. If it " +
      "stops at the same point every time, drop the transfer speed, and if " +
      "that does not help, tick \u201cErase the whole flash first\u201d. " +
      "Please report it with the log below."
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
    /*
     * With compression on, esptool reports bytes of the *compressed* stream,
     * whose size is not known until it starts each part. Track each part's
     * own completion fraction and weight it by uncompressed size, so the bar
     * means the same thing whether or not a part compresses well.
     */
    const weights = fileArray.map((f) => f.data.length);
    const weightTotal = weights.reduce((a, b) => a + b, 0);
    const fraction = new Array(fileArray.length).fill(0);

    status("Writing flash — do not unplug the board.");
    await esploader.writeFlash({
      fileArray,
      flashSize: manifest.flash.size,
      flashMode: manifest.flash.mode,
      flashFreq: manifest.flash.frequency,
      /*
       * Erasing up front is the difference between a write that finishes and
       * one that stalls. Left to itself the loader erases as it writes, and a
       * block erase that runs long makes the device miss its response window
       * — which surfaces as a short reply and a failed block, always at the
       * same place regardless of transfer speed.
       */
      eraseAll: ui.erase?.checked === true,
      compress: true,
      reportProgress: (index, written, fileTotal) => {
        fraction[index] = fileTotal > 0 ? Math.min(1, written / fileTotal) : 0;
        const weighted = fraction.reduce(
          (sum, f, i) => sum + f * weights[i],
          0
        );
        const pct = Math.min(100, Math.round((weighted / weightTotal) * 100));
        ui.bar.style.width = pct + "%";
        ui.bar.textContent = pct + "%";
      },
    });

    status("Restarting the Sidecar…");
    await restartBoard();
    /*
     * Then release the port. Native esptool ends by closing it, and closing
     * drops the control lines to their idle state, which resets the board on
     * this hardware — verified on a real Nano. The page previously held the
     * port open after writing, so that reset never happened and the Sidecar
     * sat in the bootloader until it was unplugged.
     *
     * The signal pulse above resets the board on its own over a plain serial
     * port, so it is kept, but it evidently is not enough through Web Serial.
     * Closing is also simply correct: nothing needs the port once the write
     * is verified.
     */
    await releasePort();
    status(
      "Done. The Sidecar has restarted — look for its Wi-Fi network, or its " +
      "OLED if one is fitted.",
      "ok"
    );
    return;
  } catch (error) {
    status(describe(error), "error");
    log(String(error?.stack || error));
    // The board is still attached and still in the bootloader after a failed
    // write, so allow another attempt without reconnecting.
    ui.flash.disabled = !gateOk;
  } finally {
    ui.disconnect.disabled = false;
    ui.connect.disabled = esploader !== null;
  }
}

/*
 * Pulse the board out of the bootloader.
 *
 * esptool-js's HardReset only lowers RTS:
 *
 *   async reset() { await delay(100); await this.transport.setRTS(false) }
 *
 * That assumes RTS is currently high and holding the chip in reset. Its own
 * connect sequence ends with RTS already low, so on this board the call
 * changes nothing and the Sidecar sits in the bootloader until it is power
 * cycled by hand. Passing resetConstructors was necessary but not sufficient.
 *
 * Drive the whole pulse instead: DTR low so GPIO0 stays high and the chip
 * boots the application rather than re-entering download mode, then RTS high
 * to assert EN, hold, and release. This is the sequence native esptool uses,
 * and the one that has reset this hardware reliably over a plain serial port.
 */
async function restartBoard() {
  if (!transport?.setRTS || !transport?.setDTR) {
    log("cannot restart: the transport exposes no control signals");
    return false;
  }
  const pause = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
  try {
    // setDTR(false) is IO0 high, so the chip boots the application rather
    // than re-entering download mode. setRTS(true) is EN low: in reset.
    await transport.setDTR(false);
    await transport.setRTS(true);
    await pause(150);
    await transport.setRTS(false);
    await pause(50);
    log("restarted the board (EN pulsed via RTS)");
    return true;
  } catch (error) {
    log(`could not restart the board automatically: ${error.message}`);
    return false;
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

/* Release the port without disturbing a status message already written. */
async function releasePort() {
  const message = ui.status.textContent;
  const kind = ui.status.className;
  await disconnect();
  ui.status.textContent = message;
  ui.status.className = kind;
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
