// Compile and exercise the production caption snapshot parser on the host.
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const root = path.resolve(__dirname, '..');
const output = fs.mkdtempSync(path.join(os.tmpdir(), 'agent-realtime-captions-'));
const source = path.join(output, 'captions_test.cc');
fs.writeFileSync(source, String.raw`
#include <cassert>
#include <iostream>
#include "codex_realtime_captions.h"
using agent_ui::codex_realtime::Captions;
bool Apply(Captions& captions, const char* json) {
    cJSON* root = cJSON_Parse(json);
    assert(root);
    bool result = captions.Apply(root);
    cJSON_Delete(root);
    return result;
}
int main() {
    Captions captions;
    assert(Apply(captions, R"({"sequence":1,"entries":[{"role":"user","text":"你好😀"}]})"));
    assert(captions.text == "你：你好😀");
    assert(Apply(captions, R"({"sequence":5,"entries":[{"role":"user","text":"打开设置"},{"role":"assistant","text":"已经打开。"}]})"));
    assert(captions.text == "你：打开设置\nAI：已经打开。");
    assert(captions.lines[0] == "你：打开设置" && captions.lines[1] == "AI：已经打开。");
    const std::string before = captions.text;
    for (const auto* json : {
        R"({"sequence":4,"entries":[{"role":"user","text":"旧消息"}]})",
        R"({"sequence":5,"entries":[{"role":"user","text":"重复消息"}]})",
        R"({"sequence":6.5,"entries":[{"role":"user","text":"非整数"}]})",
        R"({"sequence":4294967296,"entries":[{"role":"user","text":"溢出"}]})",
        R"({"sequence":6,"entries":[{"role":"user","text":"部分有效"},{"role":"system","text":"无效角色"}]})",
        R"({"sequence":6,"entries":[{"role":"assistant","text":123}]})",
        R"({"sequence":6,"entries":[]})",
        R"({"entries":[{"role":"user","text":"缺少序号"}]})"
    }) {
        assert(!Apply(captions, json));
        assert(captions.text == before && captions.sequence == 5);
    }
    const std::string prefix = R"({"sequence":6,"entries":[{"role":"assistant","text":")";
    assert(!Apply(captions, (prefix + std::string(2049, 'x') + R"("}]})").c_str()));
    assert(Apply(captions, (prefix + std::string(2048, 'x') + R"("}]})").c_str()));
    captions.Clear();
    assert(captions.sequence == 0 && captions.text.empty() && captions.lines[0].empty() && captions.lines[1].empty());
    assert(Apply(captions, R"({"sequence":1,"entries":[{"role":"assistant","text":"新通话"}]})"));
    assert(captions.text == "AI：新通话");
    assert(Apply(captions, R"({"sequence":2,"entries":[{"role":"assistant","text":"上一条回复"},{"role":"user","text":"新问题\n下一段\t文字"}]})"));
    assert(captions.lines[0] == "AI：上一条回复");
    assert(captions.lines[1] == "你：新问题 下一段 文字");
    using agent_ui::codex_realtime::CaptionScroll;
    CaptionScroll scroll;
    scroll.Follow(-20, 0);
    assert(scroll.Offset(5000) == 0);
    scroll.Follow(90, 5000);
    assert(scroll.Offset(5075) == 45);
    assert(scroll.Offset(5150) == 90);
    assert(scroll.Offset(15000) == 90); // No replay of already spoken text.
    scroll.Follow(12000, 15000);
    assert(scroll.Offset(15150) == 12000); // Long utterances have the same bound.
    int previous_offset = scroll.Offset(15150);
    for (uint32_t now = 15200; now <= 75200; now += 100) {
        const int overflow = 12000 + static_cast<int>(now - 15100);
        scroll.Follow(overflow, now);
        const int offset = scroll.Offset(now);
        assert(offset >= previous_offset);
        assert(overflow - offset <= 151); // A minute of growth cannot build a backlog.
        previous_offset = offset;
    }
    assert(scroll.Offset(75350) == scroll.target);
    scroll.Follow(0, 75400); // A short new utterance/correction is immediately visible.
    assert(scroll.Offset(75400) == 0);
    scroll.Follow(900, UINT32_MAX - 74);
    assert(scroll.Offset(75) == 900); // LVGL tick wrap.
    std::cout << "Caption parser: Unicode, corrections, ordering, limits, atomic rejection and new-call reset passed\n";
}
`);
const include = path.join(root, 'main/display/agent_ui/apps/codex');
const cjson = path.join(root, 'managed_components/espressif__cjson/cJSON');
const binary = path.join(output, process.platform === 'win32' ? 'captions.exe' : 'captions');
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
  execFileSync(compiler, ['/nologo', '/std:c++17', '/EHsc', '/utf-8', '/W4', '/I' + include, '/I' + cjson,
    source, path.join(cjson, 'cJSON.c'), '/Fe:' + binary], { cwd: output, env, stdio: 'inherit', windowsHide: true });
} else {
  execFileSync(process.env.CXX || 'c++', ['-std=c++17', '-Wall', '-Wextra', '-I' + include, '-I' + cjson,
    source, path.join(cjson, 'cJSON.c'), '-o', binary], { cwd: output, stdio: 'inherit' });
}
execFileSync(binary, [], { cwd: output, stdio: 'inherit', windowsHide: true });
