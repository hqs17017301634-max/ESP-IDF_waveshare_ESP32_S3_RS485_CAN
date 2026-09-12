#include <cassert>
#include <cstdio>
#include "frame_pipeline.h"
#include "drivers/mock_driver.h"

struct RejectDriver : MockDriver
{
    unsigned attempts = 0;
    bool send(const CanFrame &) override { ++attempts; return false; }
};

static bool allowAD() { return true; }

static void compose_contracts()
{
    CanFrame original{.id = 1021};
    original.data[3] = 0xA5;
    FrameContext context(original, 1, FrameProtocol::HW3, 100);
    FrameCoordinator plan(context);
    assert(plan.bits(FeatureId::Fsd, 5, 0x40, 0x40));
    assert(plan.bits(FeatureId::Profile, 6, 0x06, 0x04));
    assert(plan.bits(FeatureId::Fsd, 5, 0x40, 0x40)); // identical overlap
    TxRequest request;
    assert(plan.finalize(request) == ComposeResult::Ready);
    assert(request.frame().data[5] == 0x40 && request.frame().data[6] == 4);
    assert(request.frame().data[3] == 0xA5 && original.data[5] == 0);
    TxRequest second;
    assert(plan.finalize(second) == ComposeResult::AlreadyFinalized);
    FrameCoordinator sameSource(context);
    assert(sameSource.finalize(second) == ComposeResult::AlreadyFinalized);
    TxBroker broker;
    MockDriver driver;
    assert(broker.enqueue(request));
    assert(broker.service(driver,100,1,FrameProtocol::HW3));
    assert(!broker.enqueue(request));
    assert(driver.sent.size() == 1);

    FrameContext fresh(original, 2, FrameProtocol::HW3, 101);
    FrameCoordinator next(fresh);
    next.bits(FeatureId::Fsd, 5, 0x40, 0x40);
    TxRequest nextRequest;
    assert(next.finalize(nextRequest) == ComposeResult::Ready);
    assert(broker.enqueue(nextRequest));
    assert(broker.service(driver,101,1,FrameProtocol::HW3)); // equal payload, distinct RX
    assert(driver.sent.size() == 2);
}

static void reject_contracts()
{
    for (unsigned which = 0; which < 9; ++which)
    {
        CanFrame frame{.id = 1021};
        if (which == 2) frame.dlc = 7;
        if (which == 3) frame.id = 921;
        if (which == 4) frame.extended = true;
        if (which == 5) frame.remote = true;
        if (which == 6) frame.dlc = 9;
        if (which == 7) frame.data[0] = 2;
        if (which == 8) frame.id = 0x103FD;
        FrameContext context(frame, 1, FrameProtocol::HW3, 10);
        FrameCoordinator plan(context);
        if (which == 0) plan.bits(FeatureId::Fsd, 0, 7, 1); // mux is immutable
        else if (which == 1) plan.bits(FeatureId::Profile, 5, 0x40, 0x40); // wrong owner
        else plan.bits(FeatureId::Fsd, 5, 0x40, 0x40);
        TxRequest request;
        assert(plan.finalize(request) == ComposeResult::Invalid);
    }
    CanFrame frame{.id = 1021};
    FrameContext context(frame, 1, FrameProtocol::HW3, 10);
    FrameCoordinator conflict(context);
    assert(conflict.bits(FeatureId::Fsd, 5, 0x40, 0x40));
    assert(!conflict.bits(FeatureId::Fsd, 5, 0x40, 0));
    TxRequest request;
    assert(conflict.finalize(request) == ComposeResult::Conflict);
}

static void integrity_contracts()
{
    CanFrame frame{.id = 760};
    for (unsigned i = 0; i < 8; ++i) frame.data[i] = i * 29;
    FrameContext context(frame, 1, FrameProtocol::Legacy, 0);
    FrameCoordinator plan(context);
    plan.bits(FeatureId::LegacyMpp, 6, 0x1F, 30);
    TxRequest request;
    assert(plan.finalize(request) == ComposeResult::Ready);
    assert(request.frame().data[7] == computeVehicleChecksum(request.frame()));
    for (unsigned i = 0; i < 6; ++i) assert(request.frame().data[i] == frame.data[i]);
    assert((request.frame().data[6] & 0xE0) == (frame.data[6] & 0xE0));
    FrameContext bad(frame, 2, FrameProtocol::Legacy, 1);
    FrameCoordinator other(bad);
    assert(!other.bits(FeatureId::LegacyMpp, 7, 0xFF, 1));
    TxRequest invalid;
    assert(other.finalize(invalid) == ComposeResult::Invalid);
}

static void refresh_and_failure_contracts()
{
    CanFrame frame{.id = 1021};
    frame.data[5] = 0x40;
    frame.data[7] = 0x10;
    for (unsigned refresh = 0; refresh < 2; ++refresh)
    {
        FrameContext context(frame, 1, FrameProtocol::HW4, 0);
        FrameCoordinator plan(context);
        plan.bits(FeatureId::Fsd, 5, 0x40, 0x40,
                  refresh ? RefreshPolicy::EverySource : RefreshPolicy::ChangedOnly);
        TxRequest request;
        assert(plan.finalize(request) == (refresh ? ComposeResult::Ready : ComposeResult::NoChange));
    }
    HW4Handler hw4;
    hw4.enablePrint = false;
    hw4.checkAD = allowAD;
    forceActivateRuntime = true;
    FrameContext context(frame, 9, hw4.protocol(), 20);
    FrameCoordinator plan(context);
    hw4.collectIntents(frame, plan);
    TxRequest request;
    assert(plan.finalize(request) == ComposeResult::Ready);
    RejectDriver driver;
    TxBroker broker;
    assert(broker.enqueue(request));
    bool ok = broker.service(driver,21,1,FrameProtocol::HW4);
    hw4.onSubmitted(request, ok, 21);
    assert(!ok && hw4.framesSent == 0);
    assert(!broker.enqueue(request) && driver.attempts == 1);
}

static void speed_queue_commit_contract()
{
    hw3OffsetSlew = true;
    hw3SlewRate = 10;
    hw3OffsetLastQueuedRaw = 100;
    hw3OffsetLastQueuedMs = 1000;
    hw3OffsetHasQueued = true;
    CanFrame source{.id = 1021};
    source.data[0] = 2;
    CanFrame computed = source;
    assert(dashApplyHw3OffsetSlew(computed, source, 1100));
    assert(hw3OffsetLastQueuedRaw == 100 && hw3OffsetLastQueuedMs == 1000);
    uint8_t raw = 0;
    dashReadHw3OffsetRawShared(computed, raw);
    assert(raw == 96);
    FrameContext context(source, 1, FrameProtocol::HW3, 1100);
    FrameCoordinator plan(context);
    plan.bits(FeatureId::Hw3Speed, 0, 0xC0, computed.data[0]);
    plan.bits(FeatureId::Hw3Speed, 1, 0x3F, computed.data[1]);
    TxRequest request;
    assert(plan.finalize(request) == ComposeResult::Ready);
    HW3Handler handler;
    handler.onSubmitted(request, false, 1101);
    assert(hw3OffsetLastQueuedRaw == 100 && hw3OffsetLastQueuedMs == 1000);
    handler.onSubmitted(request, true, 1102);
    assert(hw3OffsetLastQueuedRaw == 96 && hw3OffsetLastQueuedMs == 1102);
    assert(handler.framesSent == 1);
}

int main()
{
    compose_contracts();
    reject_contracts();
    integrity_contracts();
    refresh_and_failure_contracts();
    speed_queue_commit_contract();
    std::puts("coordinator contracts: PASS (merge, ownership, integrity, source uniqueness, refresh, rejection, queue commit)");
}
