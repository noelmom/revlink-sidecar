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
const manifest = readFileSync(
  new URL('../components/revlink_sync/revlink_sync_manifest.c', import.meta.url),
  'utf8',
);
const transport = readFileSync(
  new URL(
    '../components/accessport_usb/revlink_accessport_usb.c',
    import.meta.url,
  ),
  'utf8',
);
const storage = readFileSync(
  new URL('../esp32p4/main/revlink_sd_storage.c', import.meta.url),
  'utf8',
);

test('the files API reports device presence as three-valued', () => {
  assert.ok(
    portalService.includes('\\"onDevice\\":'),
    'the files listing carries no onDevice field',
  );
  // null is the state that matters most: it is what an interrupted sync and
  // an upgraded-from-older-firmware cache both produce, and collapsing it to
  // false would tell an owner their files had been removed.
  assert.match(portalService, /"null"/);
  assert.match(portalService, /REVLINK_SYNC_PRESENCE_ON_DEVICE/);
  assert.match(portalService, /REVLINK_SYNC_PRESENCE_ABSENT/);
});

test('the portal distinguishes absent from unknown', () => {
  assert.match(portal, /file\.onDevice===true/);
  assert.match(portal, /file\.onDevice===false/);
  // Strict comparisons throughout: `!file.onDevice` would treat null as false.
  assert.ok(
    !/[^=!]==\s*(?:false|true)\s*[^=]/.test(
      portal.match(/file\.onDevice[^;]*/g)?.join(';') ?? '',
    ),
    'onDevice is compared loosely somewhere, which conflates null with false',
  );
  assert.match(portal, /Sidecar only/);
});

test('a file known to be gone is not offered a delete button', () => {
  assert.match(portal, /deletionAvailable\(\)&&file\.onDevice!==false/);
});

test('absence is only recorded from a listing that finished', () => {
  // The transport must report completeness, and must count listings rather
  // than downloads: an incremental sync stops downloading early by design and
  // that says nothing about what the device holds.
  assert.match(transport, /listings_completed == collections_expected/);
  // And a session that never reached the listing loop must not satisfy it
  // by comparing zero against zero.
  assert.match(transport, /collections_expected > 0U/);
  // The storage layer must refuse to apply an incomplete or overflowed pass.
  assert.match(storage, /if \(!complete \|\| device_scan_overflowed/);
});

test('an old manifest loads as unknown rather than as a claim', () => {
  assert.match(manifest, /REVLINK_SYNC_PRESENCE_UNKNOWN = 0|version == 3U/);
  // v1 and v2 have no presence column; they must still parse.
  assert.match(manifest, /REVLINK_SYNC_MANIFEST_HEADER_V1/);
  assert.match(manifest, /REVLINK_SYNC_MANIFEST_HEADER_V2/);
  assert.match(manifest, /REVLINK_SYNC_MANIFEST_HEADER_V3/);
});

test('re-syncing a file does not erase what a listing recorded', () => {
  // The listing runs before the downloads it triggers, so an upsert that
  // memset the entry would clear every presence flag on every sync.
  assert.match(manifest, /preserved_presence/);
});
