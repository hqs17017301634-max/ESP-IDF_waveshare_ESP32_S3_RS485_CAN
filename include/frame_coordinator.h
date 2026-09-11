#pragma once

#include <cstdint>
#include <cstring>
#include "can_helpers.h"

// Single-CAN adaptation of the reference project's masked-layer composer and
// source-generation consumption. No P4 queues, scheduler, or business fields.
enum class FrameProtocol : uint8_t { Legacy, HW3, HW4, Nag };
enum class FeatureId : uint8_t { Fsd, Profile, Hw3Speed, LegacyMpp, Ready, Isa, Nag };
enum class RefreshPolicy : uint8_t { ChangedOnly, EverySource };
enum class ComposeResult : uint8_t { Ready, NoIntent, NoChange, Invalid, Conflict, AlreadyFinalized };

struct FrameContext
{
    const CanFrame original;
    const uint64_t sourceSequence;
    const FrameProtocol protocol;
    // This timestamp is taken at software dequeue, not at physical reception.
    const uint32_t dequeuedAtMs;

    FrameContext(const CanFrame &frame, uint64_t sequence, FrameProtocol mode, uint32_t now)
        : original(frame), sourceSequence(sequence), protocol(mode), dequeuedAtMs(now) {}
    FrameContext(const FrameContext &) = delete;
    FrameContext &operator=(const FrameContext &) = delete;
private:
    friend class FrameCoordinator;
    bool finalized_ = false;
};

struct FieldIntent
{
    FeatureId featureId = FeatureId::Fsd;
    uint8_t byteMask[8] = {};
    uint8_t byteValue[8] = {};
    RefreshPolicy refresh = RefreshPolicy::ChangedOnly;
};

class TxBroker;
class FrameCoordinator;
class TxRequest
{
public:
    TxRequest() = default;
    TxRequest(const TxRequest &) = delete;
    TxRequest &operator=(const TxRequest &) = delete;
    const CanFrame &frame() const { return frame_; }
    uint64_t sourceSequence() const { return sequence_; }
    uint32_t owners() const { return owners_; }
    bool hasOwner(FeatureId owner) const { return (owners_ & (1U << unsigned(owner))) != 0; }
private:
    friend class FrameCoordinator;
    friend class TxBroker;
    CanFrame frame_{};
    uint64_t sequence_ = 0;
    uint32_t owners_ = 0;
    bool ready_ = false;
    bool consumed_ = false;
};

class FrameCoordinator
{
public:
    explicit FrameCoordinator(FrameContext &context) : context_(context) {}
    const CanFrame &source() const { return context_.original; }
    uint32_t nowMs() const { return context_.dequeuedAtMs; }

    bool submit(const FieldIntent &intent)
    {
        if (context_.finalized_ || !validSource() || unsigned(intent.featureId) > unsigned(FeatureId::Nag))
            return invalidate();
        bool any = false;
        for (uint8_t i = 0; i < 8; ++i)
        {
            const uint8_t mask = intent.byteMask[i];
            if (!mask) continue;
            any = true;
            if (i >= source().dlc || (mask & ~allowedMask(intent.featureId, i)))
                return invalidate();
            const uint8_t overlap = claimed_[i] & mask;
            if ((desired_[i] ^ intent.byteValue[i]) & overlap)
            {
                conflict_ = true;
                return false;
            }
        }
        if (!any) return invalidate();
        for (uint8_t i = 0; i < 8; ++i)
        {
            const uint8_t mask = intent.byteMask[i];
            desired_[i] = (desired_[i] & ~mask) | (intent.byteValue[i] & mask);
            claimed_[i] |= mask;
        }
        owners_ |= 1U << unsigned(intent.featureId);
        refresh_ = refresh_ || intent.refresh == RefreshPolicy::EverySource;
        ++intentCount_;
        return true;
    }

    bool bits(FeatureId owner, uint8_t byte, uint8_t mask, uint8_t value,
              RefreshPolicy policy = RefreshPolicy::ChangedOnly)
    {
        if (byte >= 8) return invalidate();
        FieldIntent intent;
        intent.featureId = owner;
        intent.byteMask[byte] = mask;
        intent.byteValue[byte] = value;
        intent.refresh = policy;
        return submit(intent);
    }

    ComposeResult finalize(TxRequest &request)
    {
        if (context_.finalized_ || request.ready_) return ComposeResult::AlreadyFinalized;
        context_.finalized_ = true;
        if (invalid_ || !validSource()) return ComposeResult::Invalid;
        if (conflict_) return ComposeResult::Conflict;
        if (!intentCount_) return ComposeResult::NoIntent;
        CanFrame candidate = source();
        for (uint8_t i = 0; i < candidate.dlc; ++i)
            candidate.data[i] = (candidate.data[i] & ~claimed_[i]) | (desired_[i] & claimed_[i]);
        // Integrity fields are reserved here. Features never write counters or
        // checksums, and unchanged OEM fields remain byte-for-byte intact.
        if (owners_ & (1U << unsigned(FeatureId::LegacyMpp)))
            candidate.data[7] = computeVehicleChecksum(candidate);
        if (owners_ & (1U << unsigned(FeatureId::Isa)))
            candidate.data[7] = computeVehicleChecksum(candidate);
        if (owners_ & (1U << unsigned(FeatureId::Nag)))
        {
            candidate.data[6] = (source().data[6] & 0xF0) | ((source().data[6] + 1) & 0x0F);
            uint16_t sum = 0x73;
            for (uint8_t i = 0; i < 7; ++i) sum += candidate.data[i];
            candidate.data[7] = static_cast<uint8_t>(sum);
        }
        if (!refresh_ && std::memcmp(candidate.data, source().data, source().dlc) == 0)
            return ComposeResult::NoChange;
        request.frame_ = candidate;
        request.sequence_ = context_.sourceSequence;
        request.owners_ = owners_;
        request.ready_ = true;
        return ComposeResult::Ready;
    }

private:
    bool invalidate() { invalid_ = true; return false; }
    bool validSource() const
    {
        return context_.sourceSequence != 0 && source().id <= 0x7FF &&
               source().dlc <= 8 && !source().extended && !source().remote;
    }
    uint8_t allowedMask(FeatureId owner, uint8_t byte) const
    {
        const auto &f = source();
        if (f.dlc != 8) return 0;
        const auto protocol = context_.protocol;
        const uint8_t mux = f.data[0] & 7;
        const bool legacy = protocol == FrameProtocol::Legacy && f.id == 1006;
        const bool hw3 = protocol == FrameProtocol::HW3 && f.id == 1021;
        const bool hw4 = protocol == FrameProtocol::HW4 && f.id == 1021;
        switch (owner)
        {
        case FeatureId::Fsd:
            if (mux != 0) return 0;
            if (byte == 5 && legacy) return 0x43; // existing non-compat smart offset bits
            if (byte == 5 && hw3) return 0x40;
            if (byte == 5 && hw4) return 0x40;
            if (byte == 7 && hw4) return 0x18; // existing optional EVD only
            break;
        case FeatureId::Profile:
            if ((legacy || hw3) && mux == 0 && byte == 6) return 0x06;
            if (hw4 && mux == 2 && byte == 7) return 0x70;
            break;
        case FeatureId::Hw3Speed:
            if (hw3 && mux == 2) return byte == 0 ? 0xC0 : byte == 1 ? 0x3F : 0;
            break;
        case FeatureId::LegacyMpp:
            if (protocol == FrameProtocol::Legacy && f.id == 760 && byte == 6) return 0x1F;
            break;
        case FeatureId::Ready:
            if (mux == 1 && (legacy || hw3 || hw4))
                return byte == 2 ? 0x08 : byte == 5 && hw4 ? 0x80 : 0;
            break;
        case FeatureId::Isa:
            if (protocol == FrameProtocol::HW4 && f.id == 921 && byte == 1) return 0x20;
            break;
        case FeatureId::Nag:
            if (protocol == FrameProtocol::Nag && f.id == 880)
                return byte == 2 ? 0x0F : byte == 3 ? 0xFF : byte == 4 ? 0x40 : 0;
            break;
        }
        return 0;
    }
    FrameContext &context_;
    uint8_t claimed_[8] = {};
    uint8_t desired_[8] = {};
    uint32_t owners_ = 0;
    uint16_t intentCount_ = 0;
    bool refresh_ = false;
    bool invalid_ = false;
    bool conflict_ = false;
};
