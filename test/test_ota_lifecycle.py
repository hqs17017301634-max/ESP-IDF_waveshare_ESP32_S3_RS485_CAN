"""Compile the production UpdateClass methods with a fake ESP OTA backend."""
import subprocess
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
CXX=ROOT/'_tools/native-toolchain/w64devkit/bin/g++.exe'

class OtaLifecycleTests(unittest.TestCase):
    def test_quiesce_precedes_flash_and_errors_resume(self):
        if not CXX.exists(): self.skipTest('project-local native compiler unavailable')
        header=(ROOT/'include/platform/espidf_runtime.h').read_text(encoding='utf-8')
        source=(ROOT/'src/espidf_runtime.cpp').read_text(encoding='utf-8')
        cls=header[header.index('class UpdateClass\n'):header.index('\nextern UpdateClass Update;')]
        methods=source[source.index('void UpdateClass::setError'):source.index('\nvoid WebServer::on(')]
        prefix=r'''
#include <cassert>
#include <string>
#include <cstdint>
#include <cstdio>
using esp_err_t=int; using esp_ota_handle_t=unsigned;
struct esp_partition_t {};
class WiFiClient {public:size_t readBytes(uint8_t*,size_t){return 0;}};
constexpr int ESP_OK=0,OTA_SIZE_UNKNOWN=-1;
static bool quiet=false,allow=true,partitionAvailable=true;
static int beginError=0,endError=0,bootError=0,resumes=0,starts=0;
static esp_partition_t partition;
const esp_partition_t *esp_ota_get_next_update_partition(void*){return partitionAvailable ? &partition : nullptr;}
int esp_ota_begin(const esp_partition_t*,int,esp_ota_handle_t *h){assert(quiet);++starts;*h=1;return beginError;}
int esp_ota_write(esp_ota_handle_t,const uint8_t*,size_t){assert(quiet);return 0;}
int esp_ota_end(esp_ota_handle_t){assert(quiet);return endError;}
int esp_ota_set_boot_partition(const esp_partition_t*){assert(quiet);return bootError;}
int esp_ota_abort(esp_ota_handle_t){return 0;}
const char *esp_err_to_name(int){return "test_error";}
bool pauseCan(){quiet=true;return allow;}
void resumeCan(){quiet=false;++resumes;}
'''
        suffix=r'''
int main(){
 UpdateClass u;u.beforeBegin=pauseCan;u.afterAbort=resumeCan;uint8_t data=0xe9;
 assert(u.begin(1));assert(quiet&&u.isRunning());assert(u.write(&data,1)==1);
 assert(u.end(true));assert(quiet&&u.isFinished()&&!u.isRunning());
 u.abort();assert(!quiet&&!u.isFinished());
 allow=false;int count=starts;assert(!u.begin(1));assert(!quiet&&starts==count);allow=true;
 beginError=1;assert(!u.begin(1));assert(!quiet&&!u.isRunning());beginError=0;
 assert(u.begin(1));endError=1;assert(!u.end(true));assert(!quiet&&!u.isFinished());endError=0;
 assert(u.begin(1));bootError=1;assert(!u.end(true));assert(!quiet&&!u.isFinished());bootError=0;
 assert(u.begin(1));u.abort();assert(!quiet&&!u.isRunning());
 count=starts;partitionAvailable=false;assert(!u.begin(1));assert(starts==count&&!quiet);
 puts("OTA lifecycle: success stays quiesced; refusal/begin/end/boot/abort failures resume");
}
'''
        out=ROOT/'.tests/ota-lifecycle';out.mkdir(parents=True,exist_ok=True)
        cpp=out/'test.cpp';exe=out/'test.exe';cpp.write_text(prefix+cls+methods+suffix,encoding='utf-8')
        r=subprocess.run([str(CXX),'-std=c++17',str(cpp),'-o',str(exe)],capture_output=True,text=True)
        self.assertEqual(r.returncode,0,r.stderr[-3000:])
        r=subprocess.run([str(exe)],capture_output=True,text=True)
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)

if __name__=='__main__':unittest.main()
