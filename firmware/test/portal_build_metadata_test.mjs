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
  // Assert the fields the portal depends on, not the exact literal: adding a
  // field to this object is a normal change and should not read as a failure.
  for (const field of ['version', 'date', 'time', 'commit']) {
    assert.ok(
      portalService.includes(`\\"${field}\\":`),
      `status build metadata is missing ${field}`,
    );
  }
});

test('the build commit comes from the build, not a hard-coded string', () => {
  // A UI-only change alters the portal compiled into the firmware without
  // changing the version, so the commit is what separates two such builds.
  // It has to come from the build system or it cannot do that job.
  assert.match(portalService, /REVLINK_GIT_COMMIT/);
  assert.match(portal, /text\('buildCommit'/);
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
