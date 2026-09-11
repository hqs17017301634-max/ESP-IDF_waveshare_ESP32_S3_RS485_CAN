#pragma once
#include "frame_pipeline.h"

// Test callers exercise the production composition/submission path. They do
// not restore the removed handler API or give handlers access to a driver.
inline void dispatchTestFrame(CarManagerBase &handler, const CanFrame &frame, CanDriver &driver)
{
    static uint64_t sequence = 0;
    static TxBroker broker;
    FrameContext context(frame, ++sequence, handler.protocol(), 0);
    FrameCoordinator plan(context);
    handler.collectIntents(frame, plan);
    submitComposedFrame(handler, plan, broker, driver, 0);
}
