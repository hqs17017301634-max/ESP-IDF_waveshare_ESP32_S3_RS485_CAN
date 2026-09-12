#include <unity.h>
#define TWAI_RX_QUEUE_LEN 64
#define TWAI_TX_QUEUE_LEN 0
#include "drivers/twai_driver.h"
void setUp(){fakeMs=0;fakeStatus={};fakeAlerts=0;fakeInstalls=0;fakeTransmits=0;fakeTxError=ESP_OK;fakeMutexBusy=false;fakeRx.clear();}
void tearDown(){}
static void zero_queue_nonblocking_and_no_busy_acceptance(){
    TWAIDriver d(15,16);TEST_ASSERT_TRUE(d.init());TEST_ASSERT_EQUAL(0,fakeTxQueue);TEST_ASSERT_EQUAL(64,fakeRxQueue);
    CanFrame f;f.id=0x3FD;
    TEST_ASSERT_EQUAL(CanDriver::SubmitResult::Accepted,d.trySend(f,d.controllerEpoch()));
    TEST_ASSERT_EQUAL(0,fakeTxWait);
    TEST_ASSERT_EQUAL(CanDriver::SubmitResult::Busy,d.trySend(f,d.controllerEpoch()));TEST_ASSERT_EQUAL(1,fakeTransmits);
    fakeStatus.msgs_to_tx=0;fakeMutexBusy=true;
    TEST_ASSERT_EQUAL(CanDriver::SubmitResult::Busy,d.trySend(f,d.controllerEpoch()));TEST_ASSERT_EQUAL(0,fakeMs);
}
static void filter_equality_preserves_controller_generation(){
    TWAIDriver d(15,16);d.init();uint32_t a[]={0x399,0x3F8,0x3FD},b[]={0x3FD,0x399,0x3F8};
    d.setFilters(a,3);auto epoch=d.controllerEpoch();auto installs=fakeInstalls;
    d.setPriorityFilters(b,3);TEST_ASSERT_EQUAL(epoch,d.controllerEpoch());TEST_ASSERT_EQUAL(installs,fakeInstalls);
    uint32_t c[]={0x399,0x3FD};d.setFilters(c,2);TEST_ASSERT_NOT_EQUAL(epoch,d.controllerEpoch());
}
static void quiesce_is_bounded_and_reports_unknown(){
    TWAIDriver d(15,16);d.init();CanFrame f;f.id=0x3FD;auto epoch=d.controllerEpoch();
    d.trySend(f,epoch);TEST_ASSERT_TRUE(d.quiesce(20));TEST_ASSERT_EQUAL(20,fakeMs);
    TEST_ASSERT_EQUAL(1,d.diagnostics().quiesceUnknown);TEST_ASSERT_EQUAL(TWAI_STATE_STOPPED,fakeStatus.state);
    TEST_ASSERT_EQUAL(CanDriver::SubmitResult::Stale,d.trySend(f,epoch));
    CanFrame rx;TEST_ASSERT_FALSE(d.read(rx));TEST_ASSERT_EQUAL(TWAI_STATE_STOPPED,fakeStatus.state);
    TEST_ASSERT_TRUE(d.resume());TEST_ASSERT_EQUAL(CanDriver::SubmitResult::Accepted,d.trySend(f,d.controllerEpoch()));
}
static void stale_generation_and_bus_off_never_transmit(){
    TWAIDriver d(15,16);d.init();CanFrame f;f.id=0x3FD;
    TEST_ASSERT_EQUAL(CanDriver::SubmitResult::Stale,d.trySend(f,0));TEST_ASSERT_EQUAL(0,fakeTransmits);
    fakeStatus.state=TWAI_STATE_BUS_OFF;
    TEST_ASSERT_EQUAL(CanDriver::SubmitResult::Failed,d.trySend(f,d.controllerEpoch()));TEST_ASSERT_EQUAL(0,fakeTransmits);
}
static void extended_and_unlisted_rx_are_rejected(){
    TWAIDriver d(15,16);d.init();uint32_t ids[]={0x3FD};d.setFilters(ids,1);
    twai_message_t unrelated;unrelated.identifier=0x111;fakeRx.push_back(unrelated);
    twai_message_t extended;extended.identifier=0x3FD;extended.extd=true;fakeRx.push_back(extended);
    twai_message_t valid;valid.identifier=0x3FD;fakeRx.push_back(valid);
    CanFrame f;TEST_ASSERT_TRUE(d.read(f));TEST_ASSERT_EQUAL(0x3FD,f.id);
    TEST_ASSERT_EQUAL(1,d.diagnostics().softwareFiltered);TEST_ASSERT_EQUAL(1,d.diagnostics().invalidRx);
}
int main(){UNITY_BEGIN();RUN_TEST(zero_queue_nonblocking_and_no_busy_acceptance);RUN_TEST(filter_equality_preserves_controller_generation);
 RUN_TEST(quiesce_is_bounded_and_reports_unknown);RUN_TEST(stale_generation_and_bus_off_never_transmit);
 RUN_TEST(extended_and_unlisted_rx_are_rejected);return UNITY_END();}
