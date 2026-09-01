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

const start = script.indexOf("function parseCsvLine");
const end = script.indexOf("function healthCacheKey");
assert.ok(start >= 0 && end > start, "Vehicle Health implementation is present");

const context = vm.createContext({
  Math,
  Number,
});
vm.runInContext(
  [
    "const COLORS=['#59baff'];",
    script.slice(start, end),
    "function nice(value){return Number.isFinite(value)?String(Math.round(value*100)/100):'—'}",
  ].join("\n"),
  context,
);

function csv(rows, mutate = (values) => values) {
  const headers = [
    "Time (sec)",
    "RPM (RPM)",
    "Vehicle Speed (mph)",
    "Throttle Pos (%)",
    "Dyn Adv Mult (DAM)",
    "Feedback Knock (°)",
    "AF Learning 1 (%)",
    "Coolant Temp (F)",
    "Battery Volts (V)",
    '"AP Info:[AP3-SUB-004 v1][Test vehicle][Reflash: Test Map.ptm]"',
  ];
  const lines = [headers.join(",")];
  for (let index = 0; index < rows; index += 1) {
    const values = mutate([
      index * 0.1,
      800,
      0,
      2,
      1,
      0,
      0,
      190,
      14.1,
      "",
    ], index);
    lines.push(values.join(","));
  }
  return lines.join("\n");
}

test("stable evidence remains healthy with strong confidence", () => {
  const parsed = context.parseLogCsv(csv(400));
  const result = context.evaluateHealth(parsed);
  assert.equal(result.score, 100);
  assert.ok(result.confidence >= 80);
  assert.deepEqual(Array.from(result.conditions), ["Idle"]);
  assert.equal(result.signals.length, 0);
  assert.match(result.apInfo, /Reflash: Test Map\.ptm/);
});

test("crossed thresholds produce attributable deterministic signals", () => {
  const parsed = context.parseLogCsv(csv(100, (values, index) => {
    values[1] = index % 2 ? 350 : 900;
    values[4] = 0.5;
    values[5] = -9;
    values[6] = 35;
    values[7] = 245;
    values[8] = 11.5;
    return values;
  }));
  const result = context.evaluateHealth(parsed);
  assert.ok(result.score < 50);
  assert.ok(result.signals.some(({ title }) => /advance multiplier/i.test(title)));
  assert.ok(result.signals.some(({ title }) => /feedback knock/i.test(title)));
  assert.ok(result.signals.some(({ title }) => /fuel correction/i.test(title)));
  assert.ok(result.signals.some(({ title }) => /coolant temperature/i.test(title)));
  assert.ok(result.signals.some(({ title }) => /charging voltage/i.test(title)));
  assert.ok(result.signals.some(({ title }) => /stall/i.test(title)));
});

test("missing channels lower confidence without inventing a fault", () => {
  const sparse = [
    "Time (sec),RPM (RPM)",
    ...Array.from({ length: 20 }, (_, index) => `${index * 0.1},800`),
  ].join("\n");
  const result = context.evaluateHealth(context.parseLogCsv(sparse));
  assert.equal(result.score, 100);
  assert.ok(result.confidence < 50);
  assert.equal(result.signals.length, 0);
});
