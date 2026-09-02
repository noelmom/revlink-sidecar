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

test('editing the catalogue attaches to the displayed dataset first', () => {
  /*
   * revlink_sd_release_device() runs at the end of every sync: it clears the
   * selection and re-initialises the in-memory manifest to empty. The portal
   * keeps showing files because the published projection survives that, so
   * for most of the time the portal is in use there are rows on screen and no
   * manifest behind them.
   *
   * Guarding on storage_device.selected therefore rejected essentially every
   * cache deletion — the button worked, the request was refused, and the file
   * stayed exactly where it was.
   */
  assert.match(storage, /static esp_err_t attach_displayed_dataset/);
  for (const fn of ['revlink_sd_forget_cached', 'revlink_sd_mark_absent']) {
    const body = storage.slice(
      storage.indexOf(`esp_err_t ${fn}(const char *path)`),
    ).slice(0, 700);
    assert.match(
      body,
      /attach_displayed_dataset\(&attached_here\)/,
      `${fn} does not attach before editing the catalogue`,
    );
    assert.match(
      body,
      /if \(attached_here\)[\s\S]{0,220}revlink_sd_release_device/,
      `${fn} attaches without detaching again`,
    );
  }
  // And the bodies must not re-apply the guard that caused this. Scoped to
  // these two: revlink_sd_download_begin needs a live session and keeps it,
  // legitimately, because a download has nowhere to write without one.
  for (const fn of ['forget_cached_locked', 'mark_absent_locked']) {
    const start = storage.indexOf(`static esp_err_t ${fn}(const char *path)\n{`);
    assert.ok(start > 0, `${fn} not found`);
    const body = storage.slice(start, storage.indexOf('\n}\n', start));
    assert.ok(
      !/storage_device\.selected/.test(body),
      `${fn} still refuses to run without a live session`,
    );
  }
});

test('neither delete scope can run while a sync rewrites the catalogue', () => {
  // Both scopes edit the same manifest, so the check belongs before the scope
  // split rather than only on the device path.
  const handler = service.slice(service.indexOf('portal_file_delete_handler'));
  const syncCheck = handler.indexOf('Wait for synchronization to finish');
  const scopeSplit = handler.indexOf('if (touch_cache && !touch_device)');
  assert.ok(syncCheck > 0 && scopeSplit > 0);
  assert.ok(
    syncCheck < scopeSplit,
    'a cache delete can run while a sync is rewriting the manifest',
  );
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
  assert.match(portal, /no recycle bin/i);
  assert.match(portal, /will not exist anywhere/i);
  // The offer has to actually download, not merely advise it.
  const run = portal.slice(portal.indexOf('async function runDelete'));
  assert.match(run, /losesLastCopy/);
  assert.match(run, /await downloadFile\(file\)/);
});

test('every destructive choice is presented at once, not as a prompt chain', () => {
  /*
   * The first version asked a sequence of confirm() questions, which made the
   * safe outcome depend on answering "cancel" to the right one. A dialog that
   * names each place and what survives it is the honest shape for a choice
   * with three outcomes.
   */
  assert.match(portal, /function openDeleteDialog/);
  assert.match(portal, /Delete from the AccessPort/);
  assert.match(portal, /Remove the Sidecar/);
  assert.match(portal, /Remove it from both/);
  for (const scope of ['device', 'sidecar', 'both']) {
    assert.ok(
      portal.includes(`runDelete(file,'${scope}')`),
      `the dialog offers no ${scope} choice`,
    );
  }
});

test('only the choices that destroy the last copy are marked grave', () => {
  const fn = portal.slice(
    portal.indexOf('function openDeleteDialog'),
    portal.indexOf('async function runDelete'),
  );
  // Deleting from the device while the Sidecar keeps a copy is not grave, and
  // dressing it in the same red as a permanent loss would train people to
  // ignore the red.
  assert.match(fn, /'Delete from the AccessPort',[\s\S]{0,220}false,/);
  // Removing the cached copy is grave exactly when the device no longer has
  // it, which is a per-file question rather than a fixed answer.
  assert.match(fn, /file\.onDevice===false,\s*\(\)=>runDelete\(file,'sidecar'\)/);
  assert.match(fn, /'Remove it from both',[\s\S]{0,260}true,/);
});

test('one row button, so a phone row stays readable', () => {
  // Two red buttons side by side wrapped onto three lines at 375px and still
  // left "Delete" and "Remove" to be told apart by guessing.
  const row = portal.slice(
    portal.indexOf('const buttons=document.createElement'),
    portal.indexOf('const editor=document.createElement'),
  );
  assert.match(row, /openDeleteDialog\(file\)/);
  assert.ok(
    !/forgetCachedFile/.test(row),
    'the row still carries a second delete button',
  );
});

test('the delete button is offered whatever the device is doing', () => {
  /*
   * deletionAvailable() gates only the AccessPort choice inside the dialog.
   * The row button itself must not sit behind it, or a file the Sidecar alone
   * holds becomes undeletable — which is exactly the state that had files
   * stuck on the card with no way to remove them.
   */
  const row = portal.slice(
    portal.indexOf('const buttons=document.createElement'),
    portal.indexOf('const editor=document.createElement'),
  );
  assert.ok(
    !/deletionAvailable\(\)/.test(row),
    'the row delete button is gated on an attached AccessPort',
  );
  const dialog = portal.slice(portal.indexOf('function openDeleteDialog'));
  assert.match(dialog, /const onDevice=deletionAvailable\(\)&&file\.onDevice!==false/);
});

test('rows are redrawn when delete availability arrives late', () => {
  /*
   * The files and the status are fetched independently and the files usually
   * land first, so rows were built while deletion still looked unavailable
   * and were never rebuilt. Delete then never appeared, which is
   * indistinguishable from the feature being missing.
   */
  assert.match(portal, /function refreshRowsIfDeletionChanged/);
  assert.match(portal, /state\.rowsDrawnForDeletion/);
  // Scope the search to renderStatus's own body by matching its braces, so
  // this does not depend on how long that function happens to be.
  const start = portal.indexOf('function renderStatus(data){');
  assert.ok(start > 0, 'renderStatus not found');
  let depth = 0;
  let end = start;
  for (let i = portal.indexOf('{', start); i < portal.length; i += 1) {
    if (portal[i] === '{') depth += 1;
    else if (portal[i] === '}') {
      depth -= 1;
      if (depth === 0) { end = i; break; }
    }
  }
  const body = portal.slice(start, end);
  assert.ok(
    body.includes('refreshRowsIfDeletionChanged()'),
    'renderStatus never re-checks the rows once the status lands',
  );
  // And loadFiles must record what it drew against, or the comparison in
  // refreshRowsIfDeletionChanged has nothing to compare to.
  const load = portal.slice(portal.indexOf('async function loadFiles'));
  assert.match(load, /state\.rowsDrawnForDeletion=deletionAvailable\(\)/);
});
