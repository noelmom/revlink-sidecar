import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import test from 'node:test';

const read = (p) => readFileSync(new URL(p, import.meta.url), 'utf8');
const portal = read('../esp32p4/main/portal/index.html');
const service = read('../esp32p4/main/revlink_portal.c');
const deleteService = read('../esp32p4/main/revlink_file_delete.c');
const storage = read('../esp32p4/main/revlink_sd_storage.c');
const manifest = read('../components/revlink_sync/revlink_sync_manifest.c');

test('the endpoint accepts a scope and defaults to the device', () => {
  assert.match(service, /X-RevLink-Delete-Scope/);
  // Absent must mean what every caller meant before the option existed.
  assert.match(service, /snprintf\(scope, sizeof\(scope\), "device"\)/);
  assert.match(service, /touch_device/);
  assert.match(service, /touch_cache/);
});

test('an unrecognised scope is refused rather than defaulted', () => {
  // Silently treating a typo as "device" would be tolerable; treating it as
  // "both" would not. Refusing avoids having to reason about which.
  assert.match(service, /Delete scope must be device, sidecar, or both/);
});

test('clearing the cache does not require an attached AccessPort', () => {
  // The whole point is emptying a card for a dataset that may not be plugged
  // in. If this check moved below the device checks the feature would be
  // useless exactly when it is needed.
  const handler = service.slice(service.indexOf('portal_file_delete_handler'));
  const cacheOnly = handler.indexOf('if (touch_cache && !touch_device)');
  const deviceCheck = handler.indexOf('Connect and identify one AccessPort');
  assert.ok(cacheOnly > 0, 'no cache-only path in the delete handler');
  assert.ok(
    cacheOnly < deviceCheck,
    'the cache-only path is gated behind the attached-device checks',
  );
});

test('the cached copy is dropped only after the device confirms', () => {
  // Dropping it first and then failing the device delete would destroy the
  // only spare copy of a file that is still on the AccessPort.
  const observe = deleteService.slice(
    deleteService.indexOf('static void observe_delete'),
  );
  const removed = observe.indexOf('REVLINK_ACCESSPORT_DELETE_REMOVED');
  const forget = observe.indexOf('revlink_sd_forget_cached');
  assert.ok(removed >= 0 && forget > removed);
  // And a failed delete must clear the intent rather than leave it armed.
  assert.match(observe, /DELETE_FAILED[\s\S]{0,200}forget_cached_copy = false/);
});

test('a shared cache object survives while another entry points at it', () => {
  // The cache is content-addressed: two paths with identical bytes share one
  // object. Unlinking on the first removal would break the second file.
  assert.match(manifest, /revlink_sync_manifest_digest_users/);
  assert.match(storage, /revlink_sync_manifest_digest_users\(sync_manifest, digest\)/);
  assert.match(storage, /if \(remaining == 0U\)/);
});

test('the catalogue is saved before the payload is unlinked', () => {
  const fn = storage.slice(storage.indexOf('esp_err_t revlink_sd_forget_cached'));
  const save = fn.indexOf('save_sync_manifest');
  const unlink = fn.indexOf('unlink(object_path)');
  assert.ok(save > 0 && unlink > save,
    'unlinking before saving can leave a manifest row pointing at nothing');
});

test('the portal warns that no copy will remain, and offers the download', () => {
  assert.match(portal, /confirmLosingLastCopy/);
  assert.match(portal, /no recycle bin/i);
  assert.match(portal, /Download it first/i);
  // The offer has to actually download, not just advise it.
  assert.match(portal, /confirmLosingLastCopy[\s\S]{0,900}await downloadFile\(file\)/);
});

test('removing both copies is a separate question from deleting one', () => {
  // Bundled into one prompt, "delete" would quietly mean "destroy".
  const fn = portal.slice(portal.indexOf('async function deleteDeviceFile'));
  assert.match(fn, /Also remove the Sidecar\\u2019s cached copy/);
  // Declining anywhere in the chain must fall back to keeping the copy.
  assert.match(fn, /both=false/);
});

test('the cache row offers removal even when no device is attached', () => {
  // deletionAvailable() gates the AccessPort button on an attached device;
  // the Sidecar button must not sit behind that same gate.
  const row = portal.slice(portal.indexOf('const buttons=document.createElement'));
  assert.match(row, /forgetCachedFile/);
  assert.ok(
    !/deletionAvailable\(\)[^\n]*forgetCachedFile/.test(row),
    'removing the cached copy is gated on an attached AccessPort',
  );
});
