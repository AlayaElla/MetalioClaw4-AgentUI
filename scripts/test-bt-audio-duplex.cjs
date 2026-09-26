const fs = require('fs');
const os = require('os');
const path = require('path');
const { execFileSync } = require('child_process');
const root = path.resolve(__dirname, '..');
const output = fs.mkdtempSync(path.join(os.tmpdir(), 'bt-audio-duplex-'));
const source = fs.readFileSync(path.join(root, 'main/boards/common/bt_audio_codec.cc'), 'utf8');
const header = fs.readFileSync(path.join(root, 'main/boards/common/bt_audio_codec.h'), 'utf8');
function section(start, end) {
  const a = source.indexOf(start), b = end ? source.indexOf(end, a + start.length) : source.length;
  if (a < 0 || b < 0) throw new Error('Missing production section: ' + start);
  return source.slice(a, b);
}
// Compile the real codec declarations and I/O/route functions with a controlled I2S driver.
fs.writeFileSync(path.join(output, 'bt_audio_codec_host.h'), header.replace(/^#include.*$/gm, ''));
fs.writeFileSync(path.join(output, 'bt_audio_codec_host.inc'), [
  section('bool BTAudioCodec::SetOutputTransportEnabled(', 'void BTAudioCodec::DeleteI2sChannels()'),
  section('bool BTAudioCodec::SetI2sClockRole(', 'void BTAudioCodec::InitializeWsClockProbe('),
  section('int BTAudioCodec::Write(', null)
].join('\n'));
const test = path.join(root, 'tests/bt_audio_duplex_test.cc');
const binary = path.join(output, process.platform === 'win32' ? 'duplex.exe' : 'duplex');
if (process.platform === 'win32') {
  const vswhere = path.join(process.env['ProgramFiles(x86)'], 'Microsoft Visual Studio/Installer/vswhere.exe');
  const installation = execFileSync(vswhere, ['-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], { encoding: 'utf8', windowsHide: true }).trim();
  const vcvars = path.join(installation, 'VC/Auxiliary/Build/vcvars64.bat');
  const exported = execFileSync('cmd.exe', ['/d', '/s', '/c', 'call "' + vcvars + '" >nul && set INCLUDE && set LIB && set PATH'], { encoding: 'utf8', windowsHide: true, windowsVerbatimArguments: true });
  const env = { ...process.env };
  for (const line of exported.split(/\r?\n/)) {
    const entry = line.match(/^([^=]+)=(.*)$/);
    if (entry && /^(INCLUDE|LIB|LIBPATH|PATH)$/i.test(entry[1])) {
      for (const key of Object.keys(env)) if (key.toLowerCase() === entry[1].toLowerCase()) delete env[key];
      env[entry[1]] = entry[2];
    }
  }
  const compiler = execFileSync('where.exe', ['cl.exe'], { env, encoding: 'utf8', windowsHide: true }).trim().split(/\r?\n/)[0];
  execFileSync(compiler, ['/nologo', '/std:c++17', '/EHsc', '/utf-8', '/W4', '/I' + output, test, '/Fe:' + binary], { cwd: output, env, stdio: 'inherit', windowsHide: true });
} else {
  execFileSync(process.env.CXX || 'c++', ['-std=c++17', '-pthread', '-I' + output, test, '-o', binary], { cwd: output, stdio: 'inherit' });
}
execFileSync(binary, [], { cwd: output, stdio: 'inherit', windowsHide: true, timeout: 15000 });
