const fs=require('fs'),os=require('os'),path=require('path'),{execFileSync}=require('child_process');
const root=path.resolve(__dirname,'..'),out=fs.mkdtempSync(path.join(os.tmpdir(),'ai-ui-ops-'));
const vswhere=path.join(process.env['ProgramFiles(x86)'],'Microsoft Visual Studio','Installer','vswhere.exe');
const install=execFileSync(vswhere,['-latest','-products','*','-requires','Microsoft.VisualStudio.Component.VC.Tools.x86.x64','-property','installationPath'],{encoding:'utf8'}).trim();
const dump=execFileSync('cmd.exe',['/d','/s','/c','call "'+path.join(install,'VC','Auxiliary','Build','vcvars64.bat')+'" >nul && set INCLUDE && set LIB && set PATH'],{encoding:'utf8',windowsHide:true,windowsVerbatimArguments:true});const env={...process.env};for(const line of dump.split(/\r?\n/)){const m=line.match(/^([^=]+)=(.*)$/);if(m&&/^(INCLUDE|LIB|LIBPATH|PATH)$/i.test(m[1])){for(const k of Object.keys(env))if(k.toLowerCase()===m[1].toLowerCase())delete env[k];env[m[1]]=m[2];}}
const cl = execFileSync('where.exe', ['cl.exe'], {env, encoding:'utf8'}).trim().split(/\r?\n/)[0];
const f = path.join(root, 'scripts/fixtures/codex_ai_provider');
const u = path.join(root, 'scripts/fixtures/ai_ui_operations');
const c = path.join(root, 'managed_components/espressif__cjson/cJSON');
const sources = [path.join(f,'test.cc'), path.join(f,'navigation.cc'), path.join(u,'runtime_stubs.cc'),
  'main/ai/ai_ui_operation.cc','main/ai/ai_availability.cc','main/ai/ai_capabilities.cc',
  'main/display/agent_ui/apps/codex/codex_ai_provider.cc'].map(file => path.resolve(root,file));
execFileSync(cl, ['/nologo','/std:c++17','/EHsc','/utf-8','/I'+f,'/I'+u,'/I'+path.join(root,'main'),
  '/I'+path.join(root,'main/display/agent_ui/apps/codex'),'/I'+c, ...sources, path.join(c,'cJSON.c'),
  '/Fe:'+path.join(out,'test.exe')], {cwd:out,env,stdio:'inherit'});
execFileSync(path.join(out,'test.exe'), [], {cwd:out,stdio:'inherit'});
