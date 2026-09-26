const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const assert = require('node:assert/strict');
const {execFileSync} = require('node:child_process');
const root = path.resolve(__dirname, '..');
const out = fs.mkdtempSync(path.join(os.tmpdir(), 'camera-recovery-'));
const read = p => fs.readFileSync(path.join(root, p), 'utf8');
const vswhere = path.join(process.env['ProgramFiles(x86)'], 'Microsoft Visual Studio/Installer/vswhere.exe');
const install = execFileSync(vswhere, ['-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], {encoding:'utf8'}).trim();
const dump = execFileSync('cmd.exe', ['/d','/s','/c','call "'+path.join(install,'VC/Auxiliary/Build/vcvars64.bat')+'" >nul && set INCLUDE && set LIB && set PATH'], {encoding:'utf8',windowsHide:true,windowsVerbatimArguments:true});
const env = {...process.env};
for (const line of dump.split(/\r?\n/)) {
  const match = line.match(/^([^=]+)=(.*)$/);
  if (!match || !/^(INCLUDE|LIB|LIBPATH|PATH)$/i.test(match[1])) continue;
  for (const key of Object.keys(env)) if (key.toLowerCase() === match[1].toLowerCase()) delete env[key];
  env[match[1]] = match[2];
}
const cl = execFileSync('where.exe', ['cl.exe'], {env,encoding:'utf8'}).trim().split(/\r?\n/)[0];
function run(name, sources, includes = []) {
  const exe = path.join(out, name+'.exe');
  execFileSync(cl, ['/nologo','/std:c++20','/EHsc','/W4', ...includes.map(p=>'/I'+p), ...sources, '/Fe:'+exe], {cwd:out,env,stdio:'inherit'});
  execFileSync(exe, [], {cwd:out,stdio:'inherit'});
}
const camera = path.join(root,'main/display/agent_ui/apps/camera');
const fixtures = path.join(root,'scripts/fixtures/camera_recovery');
run('controller', [path.join(fixtures,'controller_test.cc'), path.join(camera,'camera_controller.cc')], [fixtures,camera]);

// Compile the actual patched streaming function with fault-injected allocator
// and queue dependencies. No copy of the implementation lives in this test.
const sdio = read('managed_components/espressif__esp_hosted/host/drivers/transport/sdio/sdio_drv.c');
const start = sdio.lastIndexOf('static esp_err_t sdio_push_data_to_queue(');
const implementation = sdio.slice(start, sdio.indexOf('\n#endif', start));
assert.match(implementation, /ESP_ERR_NO_MEM/);
const sdioTest = `
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NO_MEM=0x101, MEMSET_REQUIRED=1;
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
static int allocations, queued, fail_at;
static uint8_t packet[64];
static bool is_valid_sdio_rx_packet(uint8_t* buf,uint16_t* len,uint16_t* offset) {
    *len=buf[0]; *offset=2; return *len != 0;
}
static uint8_t* sdio_buffer_alloc(int) { ++allocations; return allocations==fail_at?nullptr:packet; }
static int sdio_push_pkt_to_queue(uint8_t*,uint16_t,uint16_t) { ++queued; return 0; }
${implementation}
int main() {
    uint8_t batch[]={2,0,9,9,2,0,8,8};
    fail_at=1; assert(sdio_push_data_to_queue(batch,8)==ESP_ERR_NO_MEM); assert(queued==0);
    allocations=queued=0; fail_at=2;
    assert(sdio_push_data_to_queue(batch,8)==ESP_ERR_NO_MEM); assert(queued==1);
    allocations=queued=0; fail_at=0;
    assert(sdio_push_data_to_queue(batch,8)==ESP_OK); assert(queued==2);
    allocations=queued=0;
    assert(sdio_push_data_to_queue(batch,3)==ESP_FAIL); assert(allocations==0);
    batch[0]=0; assert(sdio_push_data_to_queue(batch,8)==ESP_FAIL); assert(allocations==0);
    puts("SDIO: allocation failure, partial batch, recovery and malformed length passed");
}`;
const sdioPath = path.join(out,'sdio.cc'); fs.writeFileSync(sdioPath,sdioTest); run('sdio',[sdioPath]);
const repository = read('main/display/agent_ui/apps/camera/camera_gallery_repository.cc');
const writeJpeg = repository.slice(repository.indexOf('bool GalleryRepository::WriteJpeg('), repository.lastIndexOf('}  // namespace'));
const storageTest = `
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
static bool mounted=true, directory_ok=true, open_ok=true, short_write=false;
static int access_error=ENOENT, close_result=0, opens=0, closes=0, removes=0;
struct SdCardManager {
    static SdCardManager& GetInstance() { static SdCardManager sd; return sd; }
    bool IsMounted() const { return mounted; }
};
static bool EnsureCameraDirectory() { return directory_ok; }
static unsigned long long esp_timer_get_time() { return 1234000; }
constexpr auto kCameraDirectory="/sdcard/DCIM/Camera";
constexpr int F_OK=0;
#define ESP_LOGE(...) ((void)0)
static int mock_access(const char*,int) { errno=access_error; return -1; }
static FILE* mock_open(const char*,const char*) { ++opens; return open_ok?reinterpret_cast<FILE*>(1):nullptr; }
static size_t mock_write(const void*,size_t,size_t size,FILE*) { return short_write?size-1:size; }
static int mock_close(FILE*) { ++closes; return close_result; }
static int mock_unlink(const char*) { ++removes; return 0; }
#define access mock_access
#define fopen mock_open
#define fwrite mock_write
#define fclose mock_close
#define unlink mock_unlink
struct GalleryRepository { bool WriteJpeg(const std::vector<uint8_t>&,std::string*) const; };
${writeJpeg}
int main() {
    GalleryRepository gallery; std::vector<uint8_t> bytes={1,2,3}; std::string path;
    assert(!gallery.WriteJpeg({},&path)); assert(opens==0);
    mounted=false; assert(!gallery.WriteJpeg(bytes,&path)); assert(opens==0); mounted=true;
    directory_ok=false; assert(!gallery.WriteJpeg(bytes,&path)); assert(opens==0); directory_ok=true;
    access_error=EIO; assert(!gallery.WriteJpeg(bytes,&path)); assert(opens==0); access_error=ENOENT;
    open_ok=false; assert(!gallery.WriteJpeg(bytes,&path)); assert(opens==1 && closes==0); open_ok=true;
    short_write=true; assert(!gallery.WriteJpeg(bytes,&path)); assert(closes==1 && removes==1 && path.empty()); short_write=false;
    close_result=-1; assert(!gallery.WriteJpeg(bytes,&path)); assert(closes==2 && removes==2 && path.empty()); close_result=0;
    assert(gallery.WriteJpeg(bytes,&path)); assert(closes==3 && removes==2 && !path.empty());
    puts("storage: missing SD, directory/open/I/O errors, short write, close failure and success passed");
}`;
const storagePath = path.join(out,'storage.cc'); fs.writeFileSync(storagePath,storageTest); run('storage',[storagePath]);
const adapter = read('main/display/agent_ui/apps/camera/camera_adapter.cc');
assert.match(adapter, /RunSave\(argument\);\s*vTaskDeleteWithCaps\(nullptr\)/);
assert.doesNotMatch(adapter.slice(adapter.indexOf('static void RunSave'), adapter.indexOf('static void SaveTask')), /vTaskDelete/);
assert.match(adapter, /xTaskCreateWithCaps[\s\S]*?MALLOC_CAP_SPIRAM \| MALLOC_CAP_8BIT/);
const backend = read('main/display/agent_ui/apps/camera/camera_capture_backend.cc');
assert.doesNotMatch(backend.slice(backend.indexOf('void RunWorker('),backend.indexOf('struct WorkerContext')), /vTaskDelete/);
const tools = read('main/display/agent_ui/core/app_mcp_tools.cc');
assert.match(tools, /self\.camera\.capture[\s\S]*?camera\.control[\s\S]*?CapabilityRegistry::Get\(\)\.Invoke\(request\)/);
console.log('camera recovery runtime and task lifetime checks passed');
