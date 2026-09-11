#pragma once
#include "frame_coordinator.h"
#include "drivers/can_driver.h"

// Synchronous single-task broker: no retained frame queue or periodic replay.
// A request is consumed even on failure; a later genuine RX gets a new sequence.
// Driver acceptance is deliberately NOT called TX completion.
class TxBroker
{
public:
    struct Diagnostics
    {
        Shared<uint32_t> composed{0}, accepted{0}, rejected{0}, duplicate{0};
        Shared<uint32_t> noIntent{0}, noChange{0}, invalid{0}, conflict{0};
    } diagnostics;

    void observeCompose(ComposeResult result)
    {
        switch (result)
        {
        case ComposeResult::NoIntent: ++diagnostics.noIntent; break;
        case ComposeResult::NoChange: ++diagnostics.noChange; break;
        case ComposeResult::Invalid: ++diagnostics.invalid; break;
        case ComposeResult::Conflict: ++diagnostics.conflict; break;
        case ComposeResult::AlreadyFinalized: ++diagnostics.duplicate; break;
        case ComposeResult::Ready: break;
        }
    }

    bool submit(TxRequest &request, CanDriver &driver)
    {
        if (!request.ready_ || request.consumed_ || request.sequence_ <= lastSequence_)
        {
            ++diagnostics.duplicate;
            return false;
        }
        request.consumed_ = true;
        lastSequence_ = request.sequence_;
        ++diagnostics.composed;
        const bool accepted = driver.sendCritical(request.frame_);
        if (accepted) ++diagnostics.accepted;
        else ++diagnostics.rejected;
        return accepted;
    }
private:
    uint64_t lastSequence_ = 0;
};
