import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import vm from "node:vm";

const portalPath = new URL(
  "../esp32p4/main/portal/index.html",
  import.meta.url,
);
const html = readFileSync(portalPath, "utf8");
const script = html.match(/<script>([\s\S]*?)<\/script>/)?.[1];
assert.ok(script, "embedded portal script is present");
assert.match(html, /id="emailLogs"[^>]*>Email selected<\/button>/);
assert.match(html, /id="downloadTray"/);
assert.match(script, /showDownloadTray\(files\)/);
assert.match(
  script,
  /function openEmailFallback\(files,subject,body\)\{showDownloadTray\(files\)/,
);

const start = script.indexOf("function csvAttachmentName");
const end = script.indexOf("function canShareFiles");
assert.ok(start >= 0 && end > start, "email draft helpers are present");

const context = vm.createContext({ Date, URL });
vm.runInContext(
  [
    "function filename(path){const parts=(path||'').split('/');return parts.at(-1)||path||'File'}",
    script.slice(start, end),
  ].join("\n"),
  context,
);

test("CSV attachments drop only the transport gzip suffix", () => {
  assert.equal(
    context.csvAttachmentName({ path: "datalog/datalog58.csv.gz" }),
    "datalog58.csv",
  );
  assert.equal(
    context.csvAttachmentName({ path: "datalog/idle-test.csv" }),
    "idle-test.csv",
  );
});

test("mail draft is concise and includes useful tuner context", () => {
  const logs = [{
    path: "datalog/datalog1.csv.gz",
    initialSyncUtc: 1_785_250_000,
    mapName: "maps/tuner-revision-3.ptm",
    note: "Warm idle after a short drive.\nOccasional stumble.",
  }];
  const device = {
    vehicle: "2018 USDM WRX MT",
    partNumber: "AP3-SUB-004",
  };
  const subject = context.emailDraftSubject(device, logs);
  const body = context.emailDraftBody(device, logs);

  assert.equal(subject, "RevLink datalogs — 2018 USDM WRX MT");
  assert.match(body, /I’ve attached 1 AccessPort datalog for review/);
  assert.match(body, /datalog1\.csv/);
  assert.match(body, /Map: tuner-revision-3\.ptm/);
  assert.match(body, /Notes: Warm idle after a short drive\. Occasional stumble\./);
  assert.match(body, /Sent from RevLink Sidecar\./);
});

test("multiple files are identified in the subject", () => {
  const subject = context.emailDraftSubject(
    { partNumber: "AP3-VLK-002" },
    [{}, {}, {}],
  );
  assert.equal(subject, "RevLink datalogs — AP3-VLK-002 (3 files)");
});

test("mailto fallback preserves spaces instead of encoding them as plus signs", () => {
  const uri = context.emailDraftUri(
    "RevLink datalogs — Test vehicle",
    "Hi,\n\nPlease review these logs.",
  );
  assert.ok(!uri.includes("+"));
  const parsed = new URL(uri);
  assert.equal(parsed.searchParams.get("subject"), "RevLink datalogs — Test vehicle");
  assert.equal(parsed.searchParams.get("body"), "Hi,\n\nPlease review these logs.");
});
