const fs = require('fs');
const os = require('os');
const path = require('path');
const {execFileSync} = require('child_process');
const root = path.resolve(__dirname, '..');
const out = fs.mkdtempSync(path.join(os.tmpdir(), 'ai-registry-'));
const vswhere = path.join(process.env['ProgramFiles(x86)'], 'Microsoft Visual Studio', 'Installer', 'vswhere.exe');
const install = execFileSync(vswhere, ['-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], {encoding:'utf8', windowsHide:true}).trim();
const dump = execFileSync('cmd.exe', ['/d', '/s', '/c', 'call "' + path.join(install, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat') + '" >nul && set INCLUDE && set LIB && set PATH'], {encoding:'utf8', windowsHide:true, windowsVerbatimArguments:true});
const env = {...process.env};
for (const line of dump.split(/\r?\n/)) {
  const match = line.match(/^([^=]+)=(.*)$/);
  if (match && /^(INCLUDE|LIB|LIBPATH|PATH)$/i.test(match[1])) {
    for (const key of Object.keys(env)) if (key.toLowerCase() === match[1].toLowerCase()) delete env[key];
    env[match[1]] = match[2];
  }
}
const compiler = execFileSync('where.exe', ['cl.exe'], {env, encoding:'utf8', windowsHide:true}).trim().split(/\r?\n/)[0];
const cjson = path.join(root, 'managed_components/espressif__cjson/cJSON');
const binary = path.join(out, 'test.exe');
try {
  execFileSync(compiler, ['/nologo', '/std:c++20', '/EHsc', '/utf-8', '/I'+path.join(root,'main'), '/I'+cjson,
    path.join(root,'tests/ai_capabilities_host/ai_capabilities_host_test.cc'),
    path.join(root,'main/ai/ai_capabilities.cc'), path.join(root,'main/ai/ai_availability.cc'),
    path.join(cjson,'cJSON.c'), '/Fe:'+binary], {cwd:out, env, stdio:'inherit', windowsHide:true});
  execFileSync(binary, [], {cwd:out, stdio:'inherit', windowsHide:true});
  const calculator = path.join(out, 'calculator.exe');
  execFileSync(compiler, ['/nologo', '/std:c++20', '/EHsc', '/utf-8',
    '/I'+path.join(root,'external_apps/sdk'), path.join(root,'scripts/fixtures/calculator_ai_action.cc'),
    '/Fe:'+calculator], {cwd:out, env, stdio:'inherit', windowsHide:true});
  execFileSync(calculator, [], {cwd:out, stdio:'inherit', windowsHide:true});
} finally {
  // out is created by mkdtemp above and contains only this check's artifacts.
  fs.rmSync(out, {recursive:true, force:true});
}
