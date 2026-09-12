#include <unity.h>
#include <vector>
#include "frame_pipeline.h"
#include "drivers/mock_driver.h"
#include "can_filter_policy.h"

void setUp() {}
void tearDown() {}
struct Sink : MockDriver {
    bool busy=false, fail=false, occupy=false;
    uint32_t epoch=1, calls=0;
    SubmitResult trySend(const CanFrame &f,uint32_t generation) override {
        ++calls;
        if (generation!=epoch) return SubmitResult::Stale;
        if (busy) return SubmitResult::Busy;
        if (fail) return SubmitResult::Failed;
        send(f); busy=occupy; return SubmitResult::Accepted;
    }
    uint32_t controllerEpoch() const override { return epoch; }
};

static bool add(TxBroker &b,uint64_t seq,FeatureId owner=FeatureId::Fsd,
                FrameProtocol protocol=FrameProtocol::HW3,uint32_t now=100,uint32_t cfg=1,uint32_t ctl=1) {
    CanFrame f; f.id=protocol==FrameProtocol::Legacy ? 0x3EE : 0x3FD;
    f.data[4]=static_cast<uint8_t>(seq);
    if (owner==FeatureId::Hw3Speed || (owner==FeatureId::Profile && protocol==FrameProtocol::HW4)) f.data[0]=2;
    if (owner==FeatureId::Ready) f.data[0]=1;
    FrameContext context(f,seq,protocol,now,cfg,ctl); FrameCoordinator plan(context);
    if (owner==FeatureId::Fsd) plan.bits(owner,5,0x40,0x40);
    else if (owner==FeatureId::Ready) plan.bits(owner,5,0x80,0x80);
    else if (owner==FeatureId::Hw3Speed) plan.bits(owner,1,0x3f,12);
    else if (protocol==FrameProtocol::HW4) plan.bits(owner,7,0x70,0x20);
    else plan.bits(owner,6,6,4);
    TxRequest request; TEST_ASSERT_EQUAL(ComposeResult::Ready,plan.finalize(request));
    return b.enqueue(request);
}
static void fsd_overtakes_pending_normal() {
    TxBroker b; Sink d;
    TEST_ASSERT_TRUE(add(b,1,FeatureId::Hw3Speed)); TEST_ASSERT_TRUE(add(b,2));
    TEST_ASSERT_TRUE(b.service(d,100,1,FrameProtocol::HW3));
    TEST_ASSERT_EQUAL(2,d.sent[0].data[4]);
    TEST_ASSERT_TRUE(b.service(d,101,1,FrameProtocol::HW3));
    TEST_ASSERT_EQUAL(1,d.sent[1].data[4]); // lower source sequence is still valid
    TEST_ASSERT_EQUAL(0,b.diagnostics.pending); TEST_ASSERT_EQUAL(0,b.diagnostics.duplicate);
}
static void accepted_frame_is_never_resubmitted() {
    TxBroker b; Sink d; d.occupy=true;
    add(b,1,FeatureId::Hw3Speed); TEST_ASSERT_TRUE(b.service(d,100,1,FrameProtocol::HW3));
    add(b,2); TEST_ASSERT_FALSE(b.service(d,100,1,FrameProtocol::HW3));
    TEST_ASSERT_FALSE(b.service(d,100,1,FrameProtocol::HW3)); TEST_ASSERT_EQUAL(2,d.calls);
    d.busy=false; TEST_ASSERT_TRUE(b.service(d,101,1,FrameProtocol::HW3));
    d.busy=false; TEST_ASSERT_FALSE(b.service(d,102,1,FrameProtocol::HW3));
    TEST_ASSERT_EQUAL(2,d.sent.size()); TEST_ASSERT_EQUAL(1,d.sent[0].data[4]); TEST_ASSERT_EQUAL(2,d.sent[1].data[4]);
    TEST_ASSERT_FALSE(add(b,2)); // duplicate admission also rejected after acceptance
}
static void hw4_ready_shares_high_class_in_source_order() {
    TxBroker b; Sink d;
    add(b,1,FeatureId::Profile,FrameProtocol::HW4);
    add(b,2,FeatureId::Ready,FrameProtocol::HW4);
    add(b,3,FeatureId::Fsd,FrameProtocol::HW4);
    for (unsigned i=0;i<3;++i) TEST_ASSERT_TRUE(b.service(d,100+i,1,FrameProtocol::HW4));
    TEST_ASSERT_EQUAL(2,d.sent[0].data[4]); TEST_ASSERT_EQUAL(3,d.sent[1].data[4]); TEST_ASSERT_EQUAL(1,d.sent[2].data[4]);
    TEST_ASSERT_EQUAL(2,b.diagnostics.highAccepted); TEST_ASSERT_EQUAL(1,b.diagnostics.normalAccepted);
}
static void normal_queue_cannot_consume_fsd_reservation() {
    TxBroker b; Sink d;
    for (unsigned i=1;i<=8;++i) TEST_ASSERT_TRUE(add(b,i,FeatureId::Hw3Speed));
    TEST_ASSERT_FALSE(add(b,9,FeatureId::Hw3Speed));
    for (unsigned i=10;i<18;++i) TEST_ASSERT_TRUE(add(b,i));
    TEST_ASSERT_FALSE(add(b,18));
    TEST_ASSERT_EQUAL(16,b.diagnostics.pending); TEST_ASSERT_EQUAL(8,b.diagnostics.pendingHigh);
    TEST_ASSERT_TRUE(b.service(d,100,1,FrameProtocol::HW3)); TEST_ASSERT_EQUAL(10,d.sent[0].data[4]);
}
static void original_deadline_survives_busy() {
    TxBroker b; Sink d; d.busy=true;
    add(b,1); b.service(d,119,1,FrameProtocol::HW3);
    d.busy=false; TEST_ASSERT_FALSE(b.service(d,120,1,FrameProtocol::HW3));
    TEST_ASSERT_EQUAL(1,b.diagnostics.expired); TEST_ASSERT_EQUAL(0,d.sent.size());
}
static void busy_retries_are_bounded_and_rate_limited() {
    TxBroker b; Sink d; d.busy=true; add(b,1);
    for (unsigned t=100;t<110;++t) for (unsigned burst=0;burst<32;++burst) b.service(d,t,1,FrameProtocol::HW3);
    TEST_ASSERT_EQUAL(TxBroker::kMaxAttempts,d.calls); TEST_ASSERT_EQUAL(1,b.diagnostics.exhausted);
    TEST_ASSERT_EQUAL(0,b.diagnostics.pending);
}
static void epochs_and_protocol_cancel_old_results() {
    for (unsigned which=0;which<3;++which) {
        TxBroker b; Sink d; add(b,1);
        if (which==1) d.epoch=2;
        TEST_ASSERT_FALSE(b.service(d,100,which==0 ? 2 : 1,which==2 ? FrameProtocol::HW4 : FrameProtocol::HW3));
        TEST_ASSERT_EQUAL(1,b.diagnostics.stale); TEST_ASSERT_EQUAL(0,d.calls);
    }
}
static void quiesce_cancels_without_replaying_on_resume() {
    TxBroker b; Sink d; add(b,1); add(b,2,FeatureId::Hw3Speed);
    b.cancelAll(102); TEST_ASSERT_EQUAL(2,b.diagnostics.canceled);
    TEST_ASSERT_FALSE(add(b,1)); TEST_ASSERT_FALSE(b.service(d,103,1,FrameProtocol::HW3));
    TEST_ASSERT_TRUE(add(b,3)); TEST_ASSERT_TRUE(b.service(d,103,1,FrameProtocol::HW3));
    TEST_ASSERT_EQUAL(1,d.sent.size());
}
static void repeated_equal_rx_is_not_payload_deduplicated() {
    TxBroker b; Sink d;
    add(b,1); b.service(d,100,1,FrameProtocol::HW3);
    add(b,257); b.service(d,100,1,FrameProtocol::HW3);
    TEST_ASSERT_EQUAL(2,d.sent.size()); TEST_ASSERT_EQUAL_MEMORY(d.sent[0].data,d.sent[1].data,8);
}
static void deadline_wrap_and_fatal_error() {
    TxBroker b; Sink d; const uint32_t start=UINT32_MAX-5;
    add(b,1,FeatureId::Fsd,FrameProtocol::HW3,start);
    TEST_ASSERT_TRUE(b.service(d,3,1,FrameProtocol::HW3));
    add(b,2,FeatureId::Fsd,FrameProtocol::HW3,start);
    TEST_ASSERT_FALSE(b.service(d,14,1,FrameProtocol::HW3)); TEST_ASSERT_EQUAL(1,b.diagnostics.expired);
    add(b,3,FeatureId::Fsd,FrameProtocol::HW3,30); d.fail=true;
    TEST_ASSERT_FALSE(b.service(d,30,1,FrameProtocol::HW3)); d.fail=false;
    TEST_ASSERT_FALSE(b.service(d,31,1,FrameProtocol::HW3)); TEST_ASSERT_EQUAL(1,d.sent.size());
}
static void speed_state_commits_on_acceptance_only() {
    HW3Handler h; TxBroker b; Sink d; d.busy=true;
    hw3OffsetLastQueuedRaw=100; hw3OffsetLastQueuedMs=50; hw3OffsetHasQueued=true;
    CanFrame f; f.id=0x3FD; f.data[0]=2;
    FrameContext c(f,1,FrameProtocol::HW3,100); FrameCoordinator p(c);
    p.bits(FeatureId::Hw3Speed,1,0x3f,12);
    submitComposedFrame(h,p,b,d,100);
    TEST_ASSERT_EQUAL(100,hw3OffsetLastQueuedRaw); TEST_ASSERT_EQUAL(50,hw3OffsetLastQueuedMs);
    d.busy=false; TEST_ASSERT_TRUE(b.service(d,105,1,FrameProtocol::HW3));
    TEST_ASSERT_EQUAL(48,hw3OffsetLastQueuedRaw); TEST_ASSERT_EQUAL(105,hw3OffsetLastQueuedMs);
    TEST_ASSERT_EQUAL(1,h.framesSent);
}
static void filters_retain_only_enabled_dependencies() {
    const uint32_t all[]={0x045,0x3EE,0x2F8,0x399,0x118,0x186,0x273,0x339,0x3F8,0x3FD,0x7FF};
    for (uint8_t mode=0;mode<3;++mode) for (unsigned bits=0;bits<8;++bits) {
        uint32_t out[16]; auto n=buildCanPriorityIds(mode,bits&1,bits&2,bits&4,all,11,out,16);
        auto has=[&](uint32_t id) { for (unsigned i=0;i<n;++i) if(out[i]==id) return true; return false; };
        TEST_ASSERT_TRUE(has(mode==0 ? 0x3EE : 0x3FD));
        TEST_ASSERT_EQUAL(bool(bits&4),has(0x273)); TEST_ASSERT_EQUAL(bool(bits&4),has(0x339));
        TEST_ASSERT_EQUAL(bool(bits&6),has(0x118)); TEST_ASSERT_EQUAL(bool(bits&2),has(0x186));
        TEST_ASSERT_FALSE(has(0x7FF));
        TEST_ASSERT_EQUAL(bool(mode==0 && (bits&1)),has(0x2F8));
    }
}
static bool gateOpen=true;
static bool gate(const TxRequest &) { return gateOpen; }
static void live_gate_closure_cancels_an_already_composed_request() {
    HW3Handler h; TxBroker b; Sink d; d.busy=true; gateOpen=true;
    CanFrame f; f.id=0x3FD;
    FrameContext c(f,1,FrameProtocol::HW3,100); FrameCoordinator p(c);
    p.bits(FeatureId::Fsd,5,0x40,0x40);
    submitComposedFrame(h,p,b,d,100,nullptr,gate);
    gateOpen=false; d.busy=false;
    TEST_ASSERT_FALSE(b.service(d,101,1,FrameProtocol::HW3));
    TEST_ASSERT_EQUAL(1,b.diagnostics.gateRejected); TEST_ASSERT_EQUAL(0,d.sent.size());
    TEST_ASSERT_EQUAL(0,h.framesSent);
}
int main() {
    UNITY_BEGIN(); RUN_TEST(fsd_overtakes_pending_normal); RUN_TEST(accepted_frame_is_never_resubmitted);
    RUN_TEST(hw4_ready_shares_high_class_in_source_order); RUN_TEST(normal_queue_cannot_consume_fsd_reservation);
    RUN_TEST(original_deadline_survives_busy); RUN_TEST(busy_retries_are_bounded_and_rate_limited);
    RUN_TEST(epochs_and_protocol_cancel_old_results); RUN_TEST(quiesce_cancels_without_replaying_on_resume);
    RUN_TEST(repeated_equal_rx_is_not_payload_deduplicated); RUN_TEST(deadline_wrap_and_fatal_error);
    RUN_TEST(speed_state_commits_on_acceptance_only); RUN_TEST(filters_retain_only_enabled_dependencies);
    RUN_TEST(live_gate_closure_cancels_an_already_composed_request);
    return UNITY_END();
}
