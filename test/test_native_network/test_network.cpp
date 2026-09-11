#include <unity.h>
#include "net/dns_wire.h"
#include "net/wifi_credentials.h"
#include <vector>
#include <string>

void setUp() {}
void tearDown() {}
static std::vector<uint8_t> query(const char *name="example.com") {
    std::vector<uint8_t> p(12,0); p[0]=0x12; p[1]=0x34; p[2]=1; p[5]=1;
    std::string s(name); size_t from=0;
    if (s!=".") while (from<s.size()) {
        size_t end=s.find('.',from); if (end==std::string::npos) end=s.size();
        p.push_back(end-from); p.insert(p.end(),s.begin()+from,s.begin()+end); from=end+1;
    }
    p.insert(p.end(),{0,0,1,0,1}); return p;
}
static void addOpt(std::vector<uint8_t> &p) {
    p[11]=1; p.insert(p.end(),{0,0,41,4,208,0,0,128,0,0,0});
}
static void test_full_width_credentials() {
    uint8_t s[32],p[64];
    std::string ssid(32,'s'), psk(64,'a');
    TEST_ASSERT_TRUE(wifi_credentials::copy(s,p,ssid.c_str(),psk.c_str()));
    TEST_ASSERT_EQUAL_UINT8('s',s[31]); TEST_ASSERT_EQUAL_UINT8('a',p[63]);
    ssid+='x'; TEST_ASSERT_FALSE(wifi_credentials::copy(s,p,ssid.c_str(),"12345678"));
    TEST_ASSERT_FALSE(wifi_credentials::copy(s,p,"phone","short"));
    psk[63]='z'; TEST_ASSERT_FALSE(wifi_credentials::copy(s,p,"phone",psk.c_str()));
    TEST_ASSERT_TRUE(wifi_credentials::copy(s,p,"phone","")); TEST_ASSERT_EQUAL_UINT8(0,p[0]);
}
static void test_query_validation() {
    auto p=query("ExAmPlE.com"); dns_wire::Question q; uint16_t cap=0; bool cache=false;
    TEST_ASSERT_TRUE(dns_wire::query(p.data(),p.size(),q,cap,cache));
    TEST_ASSERT_EQUAL_STRING("example.com",q.name); TEST_ASSERT_TRUE(cache); TEST_ASSERT_EQUAL(512,cap);
    p[5]=2; TEST_ASSERT_FALSE(dns_wire::query(p.data(),p.size(),q,cap,cache)); p[5]=1;
    p[2]|=0x80; TEST_ASSERT_FALSE(dns_wire::query(p.data(),p.size(),q,cap,cache)); p[2]=1;
    for (size_t n=0;n<p.size();++n) TEST_ASSERT_FALSE(dns_wire::query(p.data(),n,q,cap,cache));
    p=query("."); TEST_ASSERT_TRUE(dns_wire::query(p.data(),p.size(),q,cap,cache));
    TEST_ASSERT_EQUAL_STRING(".",q.name);
}
static void test_edns_reply_has_no_trailing_opt() {
    auto p=query("t.sl"); const size_t end=p.size(); addOpt(p);
    dns_wire::Question q; uint16_t cap=0; bool cache=true; uint8_t out[512];
    TEST_ASSERT_TRUE(dns_wire::query(p.data(),p.size(),q,cap,cache));
    TEST_ASSERT_EQUAL(1232,cap); TEST_ASSERT_FALSE(cache);
    TEST_ASSERT_EQUAL(end,dns_wire::reply(p.data(),p.size(),out,sizeof(out),2));
    TEST_ASSERT_EQUAL(0,dns_wire::u16(out+10)); TEST_ASSERT_EQUAL(2,out[3]&15);
    TEST_ASSERT_EQUAL(end,dns_wire::reply(p.data(),p.size(),out,sizeof(out),0,true));
    TEST_ASSERT_TRUE(out[2]&2); TEST_ASSERT_EQUAL(0,dns_wire::u16(out+6));
}
static void test_real_ttl_aging_and_opt_flags() {
    auto p=query(); const size_t end=p.size(); p[2]=0x81;p[3]=0x80;p[7]=1;
    p.insert(p.end(),{0xc0,0x0c,0,1,0,1,0,0,0,10,0,4,1,2,3,4});
    addOpt(p); const size_t optTtl=end+16+5;
    uint32_t minimum=0;
    TEST_ASSERT_TRUE(dns_wire::records(p.data(),p.size(),end,3,minimum));
    TEST_ASSERT_EQUAL_UINT32(10,minimum); TEST_ASSERT_EQUAL_UINT32(7,dns_wire::u32(p.data()+end+6));
    TEST_ASSERT_EQUAL_UINT32(0x8000,dns_wire::u32(p.data()+optTtl));
    TEST_ASSERT_TRUE(dns_wire::records(p.data(),p.size(),end,8,minimum));
    TEST_ASSERT_EQUAL_UINT32(0,dns_wire::u32(p.data()+end+6));
    p.pop_back(); TEST_ASSERT_FALSE(dns_wire::records(p.data(),p.size(),end,0,minimum));
}
static void test_malformed_lengths_and_pointers() {
    auto p=query(); const size_t end=p.size(); p[2]=0x81;p[7]=1;
    p.insert(p.end(),{0xc0,0xff,0,1,0,1,0,0,0,10,0,4,1,2,3,4});
    uint32_t minimum=0; TEST_ASSERT_FALSE(dns_wire::records(p.data(),p.size(),end,0,minimum));
    p[end+1]=12;p[end+11]=30;
    TEST_ASSERT_FALSE(dns_wire::records(p.data(),p.size(),end,0,minimum));
}
int main() {
    UNITY_BEGIN(); RUN_TEST(test_full_width_credentials); RUN_TEST(test_query_validation);
    RUN_TEST(test_edns_reply_has_no_trailing_opt); RUN_TEST(test_real_ttl_aging_and_opt_flags);
    RUN_TEST(test_malformed_lengths_and_pointers); return UNITY_END();
}
