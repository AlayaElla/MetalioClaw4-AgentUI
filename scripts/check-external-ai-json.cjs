#!/usr/bin/env node
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const root = path.resolve(__dirname, '..');
const source = path.join(root, 'scripts', 'fixtures', 'external_ai_json.c');
const sdk = path.join(root, 'external_apps', 'sdk');
const output = path.join(os.tmpdir(), 'external-ai-json-check.exe');
const object = path.join(os.tmpdir(), 'external-ai-json-check.obj');
assert.equal(process.platform, 'win32', 'This host check requires Windows MSVC.');
const command = `call "D:\\Software\\Visual Studio\\Common7\\Tools\\VsDevCmd.bat" -arch=x64 && cl /nologo /utf-8 /TC "${source}" /I "${sdk}" /Fo"${object}" /Fe:"${output}" && "${output}"`;
execFileSync('powershell.exe', ['-NoProfile', '-Command',
  `& cmd.exe /d /c '${command}'; exit $LASTEXITCODE`], { stdio: 'inherit' });
fs.rmSync(output, { force: true });
fs.rmSync(object, { force: true });
console.log('external AI JSON parser check passed');
