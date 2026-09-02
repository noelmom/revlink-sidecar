import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import test from 'node:test';

const read = (p) => readFileSync(new URL(p, import.meta.url), 'utf8');
const safety = read('../../SAFETY.md');
const site = read('../../web/site/index.html');
const readme = read('../../README.md');
const flasher = read('../../web/flash/index.html');
const nanoProfile = read('../../firmware/esp32p4/sdkconfig.nano.defaults');

/*
 * The public pages once said map writing had "not been proven on hardware,
 * which is exactly why it isn't in the published build" — while SAFETY.md's
 * own accepted list said it had been, and the shipped profile compiled it in.
 * Both halves were false, and the claim had been copied into three files.
 *
 * Safety documentation that understates what the software does is not
 * cautious, it is wrong, and it is the kind of wrong nobody reports because it
 * reads as modesty. These assertions are about internal consistency: whatever
 * the profile compiles, the pages have to describe.
 */

const writesShipped = /^CONFIG_REVLINK_ALLOW_DEVICE_WRITES=y$/m.test(nanoProfile);
const deletesShipped = /^CONFIG_REVLINK_ALLOW_DEVICE_DELETES=y$/m.test(nanoProfile);

test('the shipped profile is the one these claims are checked against', () => {
  assert.ok(writesShipped, 'the Nano profile no longer compiles writes in');
  assert.ok(deletesShipped, 'the Nano profile no longer compiles deletes in');
});

test('SAFETY.md does not contradict its own accepted list', () => {
  const accepted = safety.slice(safety.indexOf('**Accepted:**'));
  const notAccepted = accepted.slice(accepted.indexOf('**Not yet accepted'));
  assert.match(accepted, /Writing maps to an AccessPort/);
  assert.ok(
    !/Writing maps? to an AccessPort/.test(notAccepted),
    'map writing appears in both the accepted and not-accepted lists',
  );
  assert.ok(
    !safety.includes('the write path has not completed its hardware round-trip'),
    'SAFETY.md still says the write path is unproven',
  );
});

test('no public page calls a shipped capability unproven or absent', () => {
  for (const [name, text] of [
    ['the landing page', site],
    ['the README', readme],
    ['the flasher page', flasher],
  ]) {
    assert.ok(
      !/not\s+(?:been\s+)?proven on hardware/i.test(text),
      `${name} still describes a shipped capability as unproven`,
    );
    if (writesShipped) {
      assert.ok(
        !/isn't in the published build|not in the published build/i.test(text),
        `${name} still says a shipped capability is absent from the build`,
      );
    }
  }
});

test('the landing page presents writing and deletion as capabilities', () => {
  // They were previously mentioned only as caveats, which understated the
  // thing the project exists to do: an AccessPort you can use without a PC.
  assert.match(site, /\.ptm/);
  assert.match(site, /deleted/i);
});

test('gating is still described, and described as consent rather than doubt', () => {
  // Correcting the record must not quietly drop the gates from the copy.
  assert.match(site, /off until you turn them on|ship locked/i);
  assert.match(readme, /gated/i);
  assert.match(flasher, /locked/i);
});
