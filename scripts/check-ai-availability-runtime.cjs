const fs = require('fs');
const os = require('os');
const path = require('path');
const {execFileSync} = require('child_process');
const root = path.resolve(__dirname, '..');
const output = fs.mkdtempSync(path.join(os.tmpdir(), 'metalio-ai-availability-'));
const test = path.join(root, 'scripts/fixtures/ai_availability_test.cc');
const source = path.join(root, 'main/ai/ai_availability.cc');
const include = path.join(root, 'main');
const binary = path.join(output, process.platform === 'win32' ? 'test.exe' : 'test');
if (process.platform === 'win32') {
  const vswhere = path.join(process.env['ProgramFiles(x86)'], 'Microsoft Visual Studio', 'Installer', 'vswhere.exe');
  const installation = execFileSync(vswhere, ['-latest', '-products', '*', '-requires',
    'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], { encoding: 'utf8', windowsHide: true }).trim();
  const vcvars = path.join(installation, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat');
  const exported = execFileSync('cmd.exe', ['/d', '/s', '/c',
    'call "' + vcvars + '" >nul && set INCLUDE && set LIB && set PATH'],
  { encoding: 'utf8', windowsHide: true, windowsVerbatimArguments: true });
  const env = { ...process.env };
  for (const line of exported.split(/\r?\n/)) {
    const entry = line.match(/^([^=]+)=(.*)$/);
    if (entry && /^(INCLUDE|LIB|LIBPATH|PATH)$/i.test(entry[1])) {
      for (const key of Object.keys(env)) if (key.toLowerCase() === entry[1].toLowerCase()) delete env[key];
      env[entry[1]] = entry[2];
    }
  }
  const compiler = execFileSync('where.exe', ['cl.exe'], { env, encoding: 'utf8', windowsHide: true }).trim().split(/\r?\n/)[0];
  execFileSync(compiler, ['/nologo', '/std:c++17', '/EHsc', '/utf-8', '/W4', '/I'+include, test, source, '/Fe:'+binary], {cwd:output,env,stdio:'inherit',windowsHide:true});
} else {
  execFileSync(process.env.CXX || 'c++', ['-std=c++17','-pthread','-Wall','-Wextra','-I'+include,test,source,'-o',binary], {cwd:output,stdio:'inherit'});
}
execFileSync(binary, [], {cwd:output,stdio:'inherit',windowsHide:true});
