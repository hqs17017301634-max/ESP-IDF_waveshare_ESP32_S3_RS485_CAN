#pragma once
#include "handlers.h"
#include "tx_broker.h"

// One stack-local context per dequeued RX, regardless of equal ID/payload.
// No timer callback can regenerate or submit a previous source frame.
inline ComposeResult submitComposedFrame(CarManagerBase &handler, FrameCoordinator &plan,
                                         TxBroker &broker, CanDriver &driver, uint32_t queuedAtMs,
                                         uint32_t (*clockMs)() = nullptr)
{
    TxRequest request;
    const auto result = plan.finalize(request);
    broker.observeCompose(result);
    if (result == ComposeResult::Ready)
    {
        const bool accepted = broker.submit(request, driver);
        handler.onSubmitted(request, accepted, clockMs ? clockMs() : queuedAtMs);
    }
    return result;
}
