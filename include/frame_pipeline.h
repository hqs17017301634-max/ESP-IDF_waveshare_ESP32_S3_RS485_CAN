#pragma once
#include "handlers.h"
#include "tx_broker.h"

// One stack-local context per dequeued RX, regardless of equal ID/payload.
// No timer callback can regenerate or submit a previous source frame.
inline ComposeResult submitComposedFrame(CarManagerBase &handler, FrameCoordinator &plan,
                                         TxBroker &broker, CanDriver &driver, uint32_t queuedAtMs,
                                         uint32_t (*clockMs)() = nullptr,
                                         TxBroker::Eligibility eligibility = nullptr)
{
    TxRequest request;
    const auto result = plan.finalize(request);
    broker.observeCompose(result);
    if (result == ComposeResult::Ready)
    {
        broker.enqueue(request, [](void *owner, const TxRequest &r, bool accepted, uint32_t now) {
            static_cast<CarManagerBase *>(owner)->onSubmitted(r,accepted,now);
        }, &handler, eligibility);
    }
    broker.service(driver,clockMs ? clockMs() : queuedAtMs,plan.configEpoch(),handler.protocol());
    return result;
}
