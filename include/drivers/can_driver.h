#pragma once

#include "../can_frame_types.h"

struct CanDriver
{
    enum class SubmitResult : uint8_t { Accepted, Busy, Failed, Stale };
    void (*onSendFrame)(const CanFrame &, bool ok) = nullptr;

    struct Diagnostics
    {
        uint32_t rxQueueFull = 0;
        uint32_t rxFifoOverrun = 0;
        uint32_t txFailed = 0;
        uint32_t txRetry = 0;
        uint32_t busOff = 0;
        uint32_t recoverCount = 0;
        uint32_t softwareFiltered = 0;
        uint32_t invalidRx = 0;
        uint32_t lastLoopGapUs = 0;
        uint32_t maxLoopGapUs = 0;
        uint32_t hardwareAcceptedIds = 0;
        uint32_t exactFilterCount = 0;
        uint32_t quiesceUnknown = 0;
    };

    virtual bool init() = 0;
    virtual void setFilters(const uint32_t *ids, uint8_t count) = 0;
    virtual void setPriorityFilters(const uint32_t *ids, uint8_t count) { setFilters(ids, count); }
    virtual bool enableInterrupt(void (*onReady)()) = 0;
    virtual bool read(CanFrame &frame) = 0;
    virtual bool send(const CanFrame &frame) = 0;
    virtual bool sendCritical(const CanFrame &frame) { return send(frame); }
    virtual SubmitResult trySend(const CanFrame &frame, uint32_t epoch) {
        if (epoch != controllerEpoch()) return SubmitResult::Stale;
        return send(frame) ? SubmitResult::Accepted : SubmitResult::Failed;
    }
    virtual uint32_t controllerEpoch() const { return 1; }
    virtual bool quiesce(uint32_t /*drainMs*/) { return true; }
    virtual bool resume() { return true; }
    virtual Diagnostics diagnostics() const { return Diagnostics{}; }
    virtual ~CanDriver() = default;
};
