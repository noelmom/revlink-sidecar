import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import test from 'node:test';

const portal = readFileSync(
  new URL('../esp32p4/main/portal/index.html', import.meta.url),
  'utf8',
);
const portalService = readFileSync(
  new URL('../esp32p4/main/revlink_portal.c', import.meta.url),
  'utf8',
);

test('portal footer exposes firmware build and compile timestamp', () => {
  assert.match(portal, /id="buildVersion"/);
  assert.match(portal, /id="buildTimestamp"/);
  assert.match(portal, /Firmware \$\{build\.version\|\|'unknown'\}/);
  assert.match(portal, /Built \$\{build\.date\} · \$\{build\.time\}/);
});

test('status API sources build metadata from the running app image', () => {
  assert.match(portalService, /esp_app_get_description\(\)/);
  assert.ok(
    portalService.includes(
      '"\\"build\\":{\\"version\\":%s,\\"date\\":%s,\\"time\\":%s},"',
    ),
  );
});

test('device cards consistently lead with the AccessPort model', () => {
  assert.match(
    portal,
    /const selectedModel=selectedKnown\?\(accessPort\.partNumber\|\|'AccessPort'\)/,
  );
  assert.match(
    portal,
    /node\.textContent=selectedModel/,
  );
  assert.match(
    portal,
    /\[accessPort\.vehicle,accessPort\.serial\]\.filter\(Boolean\)/,
  );
});
