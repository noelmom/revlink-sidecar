import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import test from 'node:test';

const portal = readFileSync(
  new URL('../esp32p4/main/portal/index.html', import.meta.url),
  'utf8',
);
const server = readFileSync(
  new URL('../esp32p4/main/revlink_portal.c', import.meta.url),
  'utf8',
);
const backup = readFileSync(
  new URL('../esp32p4/main/revlink_backup.c', import.meta.url),
  'utf8',
);
const onboarding = readFileSync(
  new URL('../esp32p4/main/revlink_onboarding.c', import.meta.url),
  'utf8',
);

test('Settings exposes browser backup, preview, and merge restore', () => {
  assert.match(portal, /id="downloadBackup"/);
  assert.match(portal, /id="backupRestoreFile"/);
  assert.match(portal, /id="applyRestore"/);
  assert.match(portal, /Existing files with different contents are reported as conflicts/);
  assert.match(server, /"\/api\/portal\/backup"/);
  assert.match(server, /"\/api\/portal\/backup\/preview"/);
  assert.match(server, /"\/api\/portal\/backup\/restore"/);
  assert.match(onboarding, /max_uri_handlers = 48U/);
  assert.doesNotMatch(
    `${server}\n${backup}`,
    /revlink_sd_portal_snapshot_t\s+\w+\s*=\s*\{0\}/,
  );
});

test('logical backup is constrained to device datasets', () => {
  assert.match(backup, /#define BACKUP_ROOT "\/sdcard\/revlink\/devices"/);
  assert.match(backup, /"revlink\/devices\/"/);
  assert.doesNotMatch(backup, /revlink\/system\/acceptance/);
  assert.doesNotMatch(backup, /wifi_store/);
});

test('restore protects existing differing paths', () => {
  assert.match(backup, /result->conflicting_files\+\+/);
  assert.match(backup, /result->identical_files\+\+/);
  assert.match(backup, /rename\(partial, destination\)/);
  assert.doesNotMatch(backup, /unlink\(destination\)/);
});
