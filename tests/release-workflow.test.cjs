'use strict';
// One-click release: the button workflow must stay on main, require green CI, refuse an existing tag, and hand the build to the signed tag run.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const read = (f) => fs.readFileSync(path.join(__dirname, '..', '.github/workflows', f), 'utf8');

test('release button: main only, green CI required, no overwrite of a tag, builds on the tag', () => {
  const w = read('release-firmware.yml');
  assert.match(w, /on:\s*\n\s*workflow_dispatch:\s*\n/);
  assert.match(w, /if: github\.ref == 'refs\/heads\/main'/);
  assert.match(w, /MAYAP_FIRMWARE_VERSION/);
  assert.match(w, /\^\[0-9\]\+\\\.\[0-9\]\+\\\.\[0-9\]\+\$/);
  assert.match(w, /ls-remote --exit-code --tags origin/);
  assert.match(w, /for name in build reliability/);
  assert.match(w, /git tag -a/);
  assert.match(w, /gh workflow run build-firmware\.yml[^\n]*--ref/);
  assert.doesNotMatch(w, /--force|push -f|secrets\./);                                 // never force a tag, never read a secret here
});

test('a signed tag run does not publish the test artifacts, and the release steps stay tag-only', () => {
  const b = read('build-firmware.yml');
  assert.match(b, /name: Luu firmware test\n\s*if: \(github\.event_name == 'workflow_dispatch' && !startsWith\(github\.ref, 'refs\/tags\/'\)\)/);
  assert.match(b, /Luu firmware Smart ON[^\n]*\n\s*if: github\.event_name == 'workflow_dispatch' && !startsWith\(github\.ref, 'refs\/tags\/'\)/);
  for (const step of ['Ky so firmware', 'Tao GitHub Release']) {
    const i = b.indexOf('- name: ' + step);
    assert.match(b.slice(i, i + 200), /if: startsWith\(github\.ref, 'refs\/tags\/'\)/, step);
  }
});
