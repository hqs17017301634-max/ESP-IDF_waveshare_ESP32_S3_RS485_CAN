#pragma once
#include "frame_coordinator.h"
#include "drivers/can_driver.h"
#include "shared_types.h"

// One CAN task owns admission/dispatch; runtime configuration serializes with
// that task. The slots own the final bytes, never pointers to stack requests.
class TxBroker {
public:
    static constexpr uint8_t kSlotsPerClass = 8;
    static constexpr uint8_t kCapacity = 2*kSlotsPerClass;
    static constexpr uint8_t kMaxAttempts = 8;
    using Completion = void (*)(void *, const TxRequest &, bool, uint32_t);
    using Eligibility = bool (*)(const TxRequest &);
    struct Diagnostics {
        Shared<uint32_t> composed{0}, accepted{0}, rejected{0}, duplicate{0};
        Shared<uint32_t> noIntent{0}, noChange{0}, invalid{0}, conflict{0};
        Shared<uint32_t> busy{0}, expired{0}, stale{0}, full{0}, canceled{0}, exhausted{0};
        Shared<uint32_t> highAccepted{0}, normalAccepted{0}, pending{0}, pendingHigh{0};
        Shared<uint32_t> maxWaitMs{0}, highMaxWaitMs{0};
        Shared<uint32_t> gateRejected{0};
    } diagnostics;

    void observeCompose(ComposeResult result) {
        switch (result) {
        case ComposeResult::NoIntent: ++diagnostics.noIntent; break;
        case ComposeResult::NoChange: ++diagnostics.noChange; break;
        case ComposeResult::Invalid: ++diagnostics.invalid; break;
        case ComposeResult::Conflict: ++diagnostics.conflict; break;
        case ComposeResult::AlreadyFinalized: ++diagnostics.duplicate; break;
        case ComposeResult::Ready: break;
        }
    }

    static bool highPriority(const TxRequest &r) {
        const uint8_t mux = r.frame().data[0]&7;
        const bool control = (r.protocol_ == FrameProtocol::Legacy && r.frame().id == 0x3EE) ||
            ((r.protocol_ == FrameProtocol::HW3 || r.protocol_ == FrameProtocol::HW4) && r.frame().id == 0x3FD);
        return r.frame().dlc == 8 && control &&
            ((mux == 0 && r.hasOwner(FeatureId::Fsd)) ||
             (r.protocol_ == FrameProtocol::HW4 && mux == 1 && r.hasOwner(FeatureId::Ready)));
    }

    bool enqueue(TxRequest &request, Completion completion = nullptr, void *owner = nullptr, Eligibility eligibility = nullptr) {
        // Admission stays in RX order; dispatch may reorder different classes.
        if (!request.ready_ || request.consumed_ || request.sequence_ <= lastAdmitted_) {
            ++diagnostics.duplicate; return false;
        }
        request.consumed_ = true;
        lastAdmitted_ = request.sequence_;
        ++diagnostics.composed;
        const bool high = highPriority(request);
        const uint8_t start = high ? 0 : kSlotsPerClass;
        for (uint8_t i=start; i<start+kSlotsPerClass; ++i) {
            Slot &slot=slots_[i];
            if (slot.used) continue;
            TxRequest &r=slot.request;
            r.frame_=request.frame_; r.sequence_=request.sequence_; r.owners_=request.owners_;
            r.protocol_=request.protocol_; r.configEpoch_=request.configEpoch_;
            r.controllerEpoch_=request.controllerEpoch_; r.dequeuedAtMs_=request.dequeuedAtMs_;
            r.deadlineMs_=request.deadlineMs_; r.ready_=true; r.consumed_=true;
            slot.callback=completion; slot.owner=owner; slot.eligibility=eligibility; slot.used=true;
            slot.attempts=0; slot.retryAtMs=request.dequeuedAtMs_;
            ++diagnostics.pending;
            if (high) ++diagnostics.pendingHigh;
            return true;
        }
        ++diagnostics.full; ++diagnostics.rejected;
        if (completion) completion(owner,request,false,request.dequeuedAtMs_);
        return false;
    }

    bool service(CanDriver &driver, uint32_t now, uint32_t config, FrameProtocol protocol) {
        const uint32_t controller=driver.controllerEpoch();
        for (auto &slot: slots_) {
            if (!slot.used) continue;
            const auto &r=slot.request;
            if (r.configEpoch_ != config || r.controllerEpoch_ != controller || r.protocol_ != protocol) {
                ++diagnostics.stale; finish(slot,false,now);
            } else if (static_cast<int32_t>(now-r.deadlineMs_) >= 0) {
                ++diagnostics.expired; finish(slot,false,now);
            } else if (slot.eligibility && !slot.eligibility(r)) {
                ++diagnostics.gateRejected; finish(slot,false,now);
            }
        }
        Slot *next=nullptr;
        for (uint8_t group=0; group<2; ++group) {
            for (uint8_t i=group*kSlotsPerClass; i<(group+1)*kSlotsPerClass; ++i)
                if (slots_[i].used && (!next || slots_[i].request.sequence_ < next->request.sequence_)) next=&slots_[i];
            if (next) break;
        }
        if (!next || static_cast<int32_t>(now-next->retryAtMs) < 0) return false;
        ++next->attempts;
        const auto result=driver.trySend(next->request.frame_,next->request.controllerEpoch_);
        if (result == CanDriver::SubmitResult::Accepted) {
            const uint32_t age=now-next->request.dequeuedAtMs_;
            if (age > diagnostics.maxWaitMs) diagnostics.maxWaitMs=age;
            if (highPriority(next->request)) {
                ++diagnostics.highAccepted;
                if (age > diagnostics.highMaxWaitMs) diagnostics.highMaxWaitMs=age;
            } else ++diagnostics.normalAccepted;
            finish(*next,true,now); return true;
        }
        if (result == CanDriver::SubmitResult::Busy) {
            ++diagnostics.busy;
            if (next->attempts < kMaxAttempts) {
                next->retryAtMs=now+1; return false;
            }
            ++diagnostics.exhausted;
        } else if (result == CanDriver::SubmitResult::Stale) ++diagnostics.stale;
        finish(*next,false,now); return false;
    }

    void cancelAll(uint32_t now) {
        for (auto &slot: slots_) if (slot.used) {
            ++diagnostics.canceled; finish(slot,false,now);
        }
    }
private:
    struct Slot {
        TxRequest request;
        Completion callback=nullptr;
        Eligibility eligibility=nullptr;
        void *owner=nullptr;
        uint32_t retryAtMs=0;
        uint8_t attempts=0;
        bool used=false;
    } slots_[kCapacity];
    uint64_t lastAdmitted_=0;
    void finish(Slot &slot, bool accepted, uint32_t now) {
        slot.used=false;
        --diagnostics.pending;
        if (highPriority(slot.request)) --diagnostics.pendingHigh;
        if (accepted) ++diagnostics.accepted; else ++diagnostics.rejected;
        if (slot.callback) slot.callback(slot.owner,slot.request,accepted,now);
    }
};
