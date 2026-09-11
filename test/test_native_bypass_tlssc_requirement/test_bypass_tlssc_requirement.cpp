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
    bypassTlsscRequirementRuntime = kBypassTlsscRequirementDefaultEnabled;
}

void tearDown() {}

void test_native_bypass_overrides_clear_ui_selection()
{
    CanFrame f = {};
    f.data[4] = 0x00;
    TEST_ASSERT_TRUE(isADSelectedInUI(f));
}

void test_ui_bit5_selects_ad()
{
    CanFrame f = {};
    f.data[4] = 0x20;
    TEST_ASSERT_TRUE(isADSelectedInUI(f));
}

void test_bit6_does_not_select_when_bypass_disabled()
{
    CanFrame f = {};
    f.data[4] = 0x40;
    bypassTlsscRequirementRuntime = false;
    TEST_ASSERT_FALSE(isADSelectedInUI(f));
}

void test_native_hw3_bypass_requests_ad_with_clear_ui_selection()
{
    HW3Handler handler;
    handler.enablePrint = false;

    CanFrame f = {.id = 1021};
    f.data[0] = 0x00;
    f.data[4] = 0x00;

    dispatchTestFrame(handler, f, mock);

    TEST_ASSERT_TRUE(handler.ADEnabled);
    TEST_ASSERT_EQUAL(1, mock.sent.size());
}

int main()
{
    UNITY_BEGIN();

    RUN_TEST(test_native_bypass_overrides_clear_ui_selection);
    RUN_TEST(test_ui_bit5_selects_ad);
    RUN_TEST(test_bit6_does_not_select_when_bypass_disabled);
    RUN_TEST(test_native_hw3_bypass_requests_ad_with_clear_ui_selection);

    return UNITY_END();
}
