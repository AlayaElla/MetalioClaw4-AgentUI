#!/usr/bin/env node
'use strict';

const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const root = path.resolve(__dirname, '..');

function execute(command, args, options = {}) {
  const result = spawnSync(command, args, { cwd: root, stdio: 'inherit',
    windowsHide: true, ...options });
  if (result.error) throw new Error('Cannot run ' + command + ': ' + result.error.message);
  if (result.signal) throw new Error(command + ' terminated by ' + result.signal);
  if (result.status !== 0) {
    const error = new Error(command + ' failed (exit ' + result.status + ')');
    error.exitCode = result.status || 1;
    throw error;
  }
}

function powershell(script, args) {
  execute(process.env.PWSH || (process.platform === 'win32' ? 'powershell.exe' : 'pwsh'),
    ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', path.join(root, script), ...args]);
}

function main(args = process.argv.slice(2)) {
  const [task = '--help', ...extra] = args;
  if (task === '--help' || extra.includes('--help')) {
    console.log('Usage: node scripts/run.cjs <task> [arguments]\n' +
      'Device: build, package, flash, flash:preserve, monitor\n' +
      'Apps: build:apps [app-name] [PowerShell parameters]\n' +
      'Example: npm run flash:preserve -- -Port COM13 -DryRun');
    return;
  }
  const hardware = {
    build: 'scripts/hardware/package-esp32.ps1',
    package: 'scripts/hardware/package-esp32.ps1',
    flash: 'scripts/hardware/flash-esp32.ps1',
    'flash:preserve': 'scripts/hardware/flash-esp32-preserve-settings.ps1',
    monitor: 'scripts/hardware/monitor-esp32.ps1',
  };
  if (hardware[task]) return powershell(hardware[task], extra);
  if (task === 'build:apps') {
    const [app, ...parameters] = extra;
    if (!app) {
      const directory = path.join(root, 'external_apps');
      const scripts = fs.readdirSync(directory, { withFileTypes: true })
        .filter(entry => entry.isFile() && /^build-[a-z0-9-]+\.ps1$/.test(entry.name))
        .map(entry => path.join('external_apps', entry.name)).sort();
      if (!scripts.length) throw new Error('No external App build scripts found.');
      for (const script of scripts) powershell(script, []);
      return;
    }
    if (!/^[a-z][a-z0-9-]*$/.test(app)) throw new Error('Specify an App name, e.g. npm run build:apps -- calculator');
    const script = 'external_apps/build-' + app + '.ps1';
    if (!fs.existsSync(path.join(root, script))) throw new Error('Unknown App: ' + app);
    return powershell(script, parameters);
  }
  throw new Error('Unknown task: ' + task + '. Run node scripts/run.cjs --help.');
}

module.exports = { execute, main };
if (require.main === module) {
  try { main(); }
  catch (error) {
    console.error(error.message);
    process.exitCode = error.exitCode || 1;
  }
}
