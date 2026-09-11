// Differential probe compiled against both frozen baseline and migrated headers.
#include <cstdio>
#include "handlers.h"
#include "drivers/mock_driver.h"
#ifndef BASELINE
#include "frame_pipeline.h"
#endif

static bool enabled = false;
static bool allowAD() { return enabled; }
static bool noNag() { return false; }
static uint32_t rng = 0x83A619U;
static uint32_t nextRandom() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

template<class Handler> void probe(unsigned protocol, uint64_t &sequence)
{
    for (unsigned config = 0; config < 128; ++config)
    {
        Handler handler;
        handler.enablePrint = false;
        handler.checkAD = allowAD;
        handler.checkNag = noNag;
        enabled = (config & 1) != 0;
        forceActivateRuntime = enabled;
        handler.speedProfileAuto = (config & 2) != 0;
        handler.speedProfile = config % (protocol == 2 ? 5 : 3);
        hw3CustomSpeed = (config & 4) != 0;
        hw3HighSpeedEnable = (config & 8) != 0;
        hw3WireEncoding = (config >> 4) & 1;
        hw3OffsetSlew = false; // changed failure/acceptance semantics tested separately
        legacyMppOverride = (config & 4) != 0;
        legacyMppCustomEnable = (config & 8) != 0;
        legacyMppHighSpeedEnable = (config & 16) != 0;
        nagKillerRuntime = true;
        fusedSpeedLimitRaw = 0;
        MockDriver driver;
#ifndef BASELINE
        TxBroker broker;
#endif
        const uint32_t ids[] = {280,390,921,1016,2047,760,1006,1021,1021,1021,627,825,880,69};
        for (unsigned sample = 0; sample < 224; ++sample)
        {
            CanFrame original;
            original.id = ids[sample % 14];
            for (auto &byte : original.data) byte = static_cast<uint8_t>(nextRandom());
            if (original.id == 1006 || original.id == 1021)
                original.data[0] = (original.data[0] & 0xF8) | ((sample / 14) % 4);
            if (original.id == 2047) original.data[0] = (original.data[0] & 0xF8) | 2;
            // Every test also feeds a second identical genuine RX event.
            for (unsigned repeat = 0; repeat < 2; ++repeat)
            {
                driver.reset();
                ++sequence;
#ifdef BASELINE
                auto frame = original;
                handler.handleMessage(frame, driver);
#if defined(ESP32_DASHBOARD) && DASH_FSD_252_COMPAT
                const bool gate = !(config & 32) || handler.injectionGateOpen();
                if (protocol < 2 && enabled && gate && original.dlc == 8 &&
                    original.id == (protocol == 0 ? 1006U : 1021U) && readMuxID(original) == 0)
                {
                    auto modified = original;
                    if (!handler.speedProfileAuto) setSpeedProfileV12V13(modified, handler.speedProfile);
                    setBit(modified, 46, true);
                    if (framePayloadChanged(original, modified)) driver.sendCritical(modified);
                }
#endif
#else
                FrameContext context(original, sequence, handler.protocol(), static_cast<uint32_t>(sequence));
                FrameCoordinator plan(context);
                handler.collectIntents(original, plan);
                collectFsdCompatibility(handler, plan, enabled && (!(config & 32) || handler.injectionGateOpen()));
                submitComposedFrame(handler, plan, broker, driver, static_cast<uint32_t>(sequence));
#endif
                std::printf("%u %u %u %u %zu",protocol,config,sample,repeat,driver.sent.size());
                for (const auto &sent : driver.sent)
                {
                    std::printf(" %03X/%u/",unsigned(sent.id),unsigned(sent.dlc));
                    for (auto byte : sent.data) std::printf("%02X",byte);
                }
                std::printf("\n");
            }
        }
    }
}
int main()
{
    uint64_t sequence = 0;
    probe<LegacyHandler>(0, sequence);
    probe<HW3Handler>(1, sequence);
    probe<HW4Handler>(2, sequence);
    probe<NagHandler>(3, sequence);
}
