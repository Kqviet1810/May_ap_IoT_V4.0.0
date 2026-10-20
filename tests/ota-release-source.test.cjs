'use strict';
// The OTA Worker reads "latest release" from ONE GitHub repository. After the repository was renamed it kept pointing at the old
// one (May_ap_trung_V2.1.1), so devices were offered the old repository's v4.0.0 instead of this repository's releases. Pin it.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const source = fs.readFileSync(path.join(__dirname, '..', 'cloudflare', 'src', 'index.js'), 'utf8');
const owner = /const GITHUB_OWNER = '([^']+)';/.exec(source)?.[1];
const repo = /const GITHUB_REPO = '([^']+)';/.exec(source)?.[1];

test('OTA release source is this repository', () => {
  assert.equal(`${owner}/${repo}`, 'Kqviet1810/May_ap_IoT_V4.0.0');
});

test('OTA release source matches the CI repository when CI provides it', (t) => {
  const ci = process.env.GITHUB_REPOSITORY;
  if (!ci) { t.skip('GITHUB_REPOSITORY not set (local run)'); return; }
  // Forks run their own CI; only enforce for the upstream repository.
  if (ci.toLowerCase() !== 'kqviet1810/may_ap_iot_v4.0.0') { t.skip(`CI repository ${ci} is not the upstream`); return; }
  assert.equal(`${owner}/${repo}`.toLowerCase(), ci.toLowerCase());
});

test('the release endpoint is built from the pinned owner/repo only', () => {
  assert.match(source, /`\/repos\/\$\{GITHUB_OWNER\}\/\$\{GITHUB_REPO\}\/releases\/latest`/);
  assert.equal((source.match(/\/releases\/latest/g) || []).length, 1);
});
