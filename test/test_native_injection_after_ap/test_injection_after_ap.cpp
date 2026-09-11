#include "../frame_test_dispatch.h"
#include <unity.h>
#include "can_frame_types.h"
#include "can_helpers.h"
#include "drivers/mock_driver.h"
#include "handlers.h"

static MockDriver mock;

void setUp()
{
    mock.reset();
    enhancedAutopilotRuntime = true;
}

void tearDown() {}

static CanFrame hw3Mux1Frame()
{
    CanFrame f = {.id = 1021};
    f.data[0] = 0x01;
    setBit(f, 19, true);
    return f;
}

static CanFrame hw4Mux1Frame()
{
    CanFrame f = {.id = 1021};
    f.data[0] = 0x01;
    setBit(f, 19, true);
    return f;
}

static CanFrame gearFrame(uint8_t gear)
{
    CanFrame f = {.id = 390};
    f.dlc = 8;
    f.data[7] = static_cast<uint8_t>(gear << 3);
    return f;
}

static CanFrame diSystemStatusFrame(uint8_t gear, bool aca)
{
    CanFrame f = {.id = 280};
    f.dlc = 8;
    f.data[2] = static_cast<uint8_t>(gear << 5);
    if (aca)
        f.data[6] = 0x04;
    return f;
}

static CanFrame summonRequestFrame()
{
    CanFrame f = {.id = 1016};
    f.dlc = 8;
    f.data[3] = 0xB0; // SMART_SUMMON
    return f;
}

static void activateAp(CarManagerBase &handler)
{
    CanFrame f = {.id = 921};
    f.data[0] = 0x03; // ACTIVE_1
    dispatchTestFrame(handler, f, mock);
    TEST_ASSERT_TRUE(handler.APActive);
    mock.reset();
}

void test_hw3_builtin_mux1_is_independent_of_ap_state()
{
    HW3Handler handler;
    handler.enablePrint = false;

    CanFrame drive = gearFrame(4);
    dispatchTestFrame(handler, drive, mock);
    TEST_ASSERT_FALSE(handler.Parked);

    CanFrame beforeAp = hw3Mux1Frame();
    dispatchTestFrame(handler, beforeAp, mock);
    TEST_ASSERT_EQUAL(1, mock.sent.size());

    CanFrame observedUiConfig = {.id = 1021};
    observedUiConfig.data[0] = 0x00;
    observedUiConfig.data[4] = 0x20;
    dispatchTestFrame(handler, observedUiConfig, mock);
    TEST_ASSERT_TRUE(handler.ADEnabled);
    TEST_ASSERT_FALSE(handler.APActive);
    mock.reset();

    CanFrame stillBeforeAp = hw3Mux1Frame();
    dispatchTestFrame(handler, stillBeforeAp, mock);
    TEST_ASSERT_EQUAL(1, mock.sent.size());

    activateAp(handler);

    CanFrame afterAp = hw3Mux1Frame();
    dispatchTestFrame(handler, afterAp, mock);
    TEST_ASSERT_EQUAL(1, mock.sent.size());
    TEST_ASSERT_FALSE((mock.sent[0].data[2] >> 3) & 0x01);
}

void test_hw3_enhanced_autopilot_allows_mux1_injection_while_parked()
{
    HW3Handler handler;
    handler.enablePrint = false;

    CanFrame park = gearFrame(1);
    dispatchTestFrame(handler, park, mock);
    TEST_ASSERT_TRUE(handler.Parked);
    TEST_ASSERT_FALSE(handler.APActive);

    CanFrame whileParked = hw3Mux1Frame();
    dispatchTestFrame(handler, whileParked, mock);

    TEST_ASSERT_EQUAL(1, mock.sent.size());
    TEST_ASSERT_FALSE((mock.sent[0].data[2] >> 3) & 0x01);
}

void test_hw3_builtin_mux1_remains_enabled_in_drive()
{
    HW3Handler handler;
    handler.enablePrint = false;

    CanFrame park = gearFrame(1);
    dispatchTestFrame(handler, park, mock);
    CanFrame whileParked = hw3Mux1Frame();
    dispatchTestFrame(handler, whileParked, mock);
    TEST_ASSERT_EQUAL(1, mock.sent.size());
    mock.reset();

    CanFrame drive = gearFrame(4);
    dispatchTestFrame(handler, drive, mock);
    TEST_ASSERT_FALSE(handler.Parked);

    CanFrame whileDriving = hw3Mux1Frame();
    dispatchTestFrame(handler, whileDriving, mock);
    TEST_ASSERT_EQUAL(1, mock.sent.size());
}

void test_hw3_summon_request_survives_aca_while_still_in_park()
{
    HW3Handler handler;
    handler.enablePrint = false;

    CanFrame requestBeforeAca = summonRequestFrame();
    dispatchTestFrame(handler, requestBeforeAca, mock);

    CanFrame acaPark = diSystemStatusFrame(1, true);
    dispatchTestFrame(handler, acaPark, mock);

    CanFrame requestDuringAca = summonRequestFrame();
    dispatchTestFrame(handler, requestDuringAca, mock);

    CanFrame stillParkedDuringAca = diSystemStatusFrame(1, true);
    dispatchTestFrame(handler, stillParkedDuringAca, mock);

    CanFrame driveDuringAca = diSystemStatusFrame(4, true);
    dispatchTestFrame(handler, driveDuringAca, mock);
    TEST_ASSERT_FALSE(handler.Parked);
    TEST_ASSERT_TRUE(handler.Summoning);

    CanFrame whileSummoning = hw3Mux1Frame();
    dispatchTestFrame(handler, whileSummoning, mock);
    TEST_ASSERT_EQUAL(1, mock.sent.size());
}

void test_hw4_builtin_mux1_requires_ad_not_ap_active()
{
    HW4Handler handler;
    handler.enablePrint = false;

    CanFrame drive = gearFrame(4);
    dispatchTestFrame(handler, drive, mock);
    TEST_ASSERT_FALSE(handler.Parked);

    CanFrame beforeAp = hw4Mux1Frame();
    dispatchTestFrame(handler, beforeAp, mock);
    TEST_ASSERT_EQUAL(0, mock.sent.size());

    CanFrame observedUiConfig = {.id = 1021};
    observedUiConfig.data[0] = 0x00;
    observedUiConfig.data[4] = 0x20;
    dispatchTestFrame(handler, observedUiConfig, mock);
    TEST_ASSERT_TRUE(handler.ADEnabled);
    TEST_ASSERT_FALSE(handler.APActive);
    mock.reset();

    CanFrame stillBeforeAp = hw4Mux1Frame();
    dispatchTestFrame(handler, stillBeforeAp, mock);
    TEST_ASSERT_EQUAL(1, mock.sent.size());

    activateAp(handler);

    CanFrame afterAp = hw4Mux1Frame();
    dispatchTestFrame(handler, afterAp, mock);
    TEST_ASSERT_EQUAL(1, mock.sent.size());
    TEST_ASSERT_FALSE((mock.sent[0].data[2] >> 3) & 0x01);
    TEST_ASSERT_EQUAL_HEX8(0x80, mock.sent[0].data[5] & 0x80);
}

void test_hw4_enhanced_autopilot_allows_mux1_injection_while_parked()
{
    HW4Handler handler;
    handler.enablePrint = false;

    CanFrame park = gearFrame(1);
    dispatchTestFrame(handler, park, mock);
    TEST_ASSERT_TRUE(handler.Parked);
    TEST_ASSERT_FALSE(handler.APActive);

    CanFrame observedUiConfig = {.id = 1021};
    observedUiConfig.data[0] = 0x00;
    observedUiConfig.data[4] = 0x20;
    dispatchTestFrame(handler, observedUiConfig, mock);
    TEST_ASSERT_TRUE(handler.ADEnabled);
    mock.reset();

    CanFrame whileParked = hw4Mux1Frame();
    dispatchTestFrame(handler, whileParked, mock);

    TEST_ASSERT_EQUAL(1, mock.sent.size());
    TEST_ASSERT_FALSE((mock.sent[0].data[2] >> 3) & 0x01);
    TEST_ASSERT_EQUAL_HEX8(0x80, mock.sent[0].data[5] & 0x80);
}

void test_hw4_builtin_mux1_with_ad_remains_enabled_in_drive()
{
    HW4Handler handler;
    handler.enablePrint = false;

    CanFrame park = gearFrame(1);
    dispatchTestFrame(handler, park, mock);
    CanFrame observedUiConfig = {.id = 1021};
    observedUiConfig.data[0] = 0x00;
    observedUiConfig.data[4] = 0x20;
    dispatchTestFrame(handler, observedUiConfig, mock);
    TEST_ASSERT_TRUE(handler.ADEnabled);
    mock.reset();

    CanFrame whileParked = hw4Mux1Frame();
    dispatchTestFrame(handler, whileParked, mock);
    TEST_ASSERT_EQUAL(1, mock.sent.size());
    mock.reset();

    CanFrame drive = gearFrame(4);
    dispatchTestFrame(handler, drive, mock);
    TEST_ASSERT_FALSE(handler.Parked);

    CanFrame whileDriving = hw4Mux1Frame();
    dispatchTestFrame(handler, whileDriving, mock);
    TEST_ASSERT_EQUAL(1, mock.sent.size());
}

void test_hw4_summon_request_survives_aca_while_still_in_park()
{
    HW4Handler handler;
    handler.enablePrint = false;

    CanFrame requestBeforeAca = summonRequestFrame();
    dispatchTestFrame(handler, requestBeforeAca, mock);

    CanFrame acaPark = diSystemStatusFrame(1, true);
    dispatchTestFrame(handler, acaPark, mock);

    CanFrame requestDuringAca = summonRequestFrame();
    dispatchTestFrame(handler, requestDuringAca, mock);

    CanFrame stillParkedDuringAca = diSystemStatusFrame(1, true);
    dispatchTestFrame(handler, stillParkedDuringAca, mock);

    CanFrame driveDuringAca = diSystemStatusFrame(4, true);
    dispatchTestFrame(handler, driveDuringAca, mock);
    TEST_ASSERT_FALSE(handler.Parked);
    TEST_ASSERT_TRUE(handler.Summoning);

    CanFrame observedUiConfig = {.id = 1021};
    observedUiConfig.data[0] = 0x00;
    observedUiConfig.data[4] = 0x20;
    dispatchTestFrame(handler, observedUiConfig, mock);
    TEST_ASSERT_TRUE(handler.ADEnabled);
    mock.reset();

    CanFrame whileSummoning = hw4Mux1Frame();
    dispatchTestFrame(handler, whileSummoning, mock);
    TEST_ASSERT_EQUAL(1, mock.sent.size());
}

int main()
{
    UNITY_BEGIN();

    RUN_TEST(test_hw3_builtin_mux1_is_independent_of_ap_state);
    RUN_TEST(test_hw3_enhanced_autopilot_allows_mux1_injection_while_parked);
    RUN_TEST(test_hw3_builtin_mux1_remains_enabled_in_drive);
    RUN_TEST(test_hw3_summon_request_survives_aca_while_still_in_park);
    RUN_TEST(test_hw4_builtin_mux1_requires_ad_not_ap_active);
    RUN_TEST(test_hw4_enhanced_autopilot_allows_mux1_injection_while_parked);
    RUN_TEST(test_hw4_builtin_mux1_with_ad_remains_enabled_in_drive);
    RUN_TEST(test_hw4_summon_request_survives_aca_while_still_in_park);

    return UNITY_END();
}
