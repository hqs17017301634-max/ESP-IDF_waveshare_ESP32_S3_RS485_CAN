#pragma once

#include "../can_frame_types.h"
#include "can_driver.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcpp"
#include <driver/twai.h>
#pragma GCC diagnostic pop
#ifdef ESP_PLATFORM
#include <esp_timer.h>
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#ifndef TWAI_RX_QUEUE_LEN
#define TWAI_RX_QUEUE_LEN 32
#endif
#ifndef TWAI_TX_QUEUE_LEN
#define TWAI_TX_QUEUE_LEN 16
#endif
#ifndef TWAI_READ_DRAIN_BUDGET
#define TWAI_READ_DRAIN_BUDGET TWAI_RX_QUEUE_LEN
#endif

class TWAIDriver : public CanDriver
{
public:
    static constexpr bool kSupportsISR = false;

    TWAIDriver(gpio_num_t txPin, gpio_num_t rxPin)
        : txPin_(txPin), rxPin_(rxPin) {}

    bool init() override
    {
        if (!mutex_)
            mutex_ = xSemaphoreCreateMutex();
        if (!mutex_)
            return false;

        g_config_ = TWAI_GENERAL_CONFIG_DEFAULT(txPin_, rxPin_, TWAI_MODE_NORMAL);
        g_config_.rx_queue_len = TWAI_RX_QUEUE_LEN;
        g_config_.tx_queue_len = TWAI_TX_QUEUE_LEN;

        t_config_ = TWAI_TIMING_CONFIG_500KBITS();
        f_config_ = TWAI_FILTER_CONFIG_ACCEPT_ALL();

        lock();
        driverOK_ = installAndStartLocked();
        unlock();
        return driverOK_;
    }

    void setFilters(const uint32_t *ids, uint8_t count) override
    {
        if (count == 0)
            return;

        twai_filter_config_t nextFilter = f_config_;
        configureMaskFilter(nextFilter, ids, count);

        lock();
        // TWAI only has a mask filter, so sparse ID sets can pass false positives.
        exactFilterCount_ = (count < kMaxExactFilters) ? count : kMaxExactFilters;
        for (uint8_t i = 0; i < exactFilterCount_; i++)
            exactFilterIds_[i] = ids[i];
        diagnostics_.exactFilterCount = exactFilterCount_;
        f_config_ = nextFilter;
        stopAndUninstallLocked();
        driverOK_ = installAndStartLocked();
        unlock();
    }

    bool enableInterrupt(void (* /*onReady*/)()) override { return false; }

    void setPriorityFilters(const uint32_t *ids, uint8_t count) override
    {
        if (count == 0)
            return;

        twai_filter_config_t nextFilter = f_config_;
        configureMaskFilter(nextFilter, ids, count);

        lock();
        // Narrow the software whitelist to the priority set as well. TWAI's
        // single mask filter cannot precisely reject the dropped diagnostic
        // IDs, so without this they would still pass software filtering and
        // reach the handler — i.e. the priority filter would be a no-op.
        exactFilterCount_ = (count < kMaxExactFilters) ? count : kMaxExactFilters;
        for (uint8_t i = 0; i < exactFilterCount_; i++)
            exactFilterIds_[i] = ids[i];
        diagnostics_.exactFilterCount = exactFilterCount_;
        f_config_ = nextFilter;
        stopAndUninstallLocked();
        driverOK_ = installAndStartLocked();
        unlock();
    }

    bool read(CanFrame &frame) override
    {
        recordLoopGap();
        serviceAlerts();
        for (uint16_t attempt = 0; attempt < kReadDrainBudget; attempt++)
        {
            lock();
            if (!driverOK_)
            {
                tryRecover();
                unlock();
                return false;
            }

            twai_message_t msg;
            if (twai_receive(&msg, 0) != ESP_OK)
            {
                if (isBusOff())
                    recoverWithCooldownLocked();
                unlock();
                return false;
            }
            bool accepted = exactFilterMatchesLocked(msg.identifier);
            unlock();

            if (!accepted)
            {
                diagnostics_.softwareFiltered++;
                continue;
            }

            if (msg.extd || msg.rtr || msg.data_length_code > 8)
            {
                ++diagnostics_.invalidRx;
                continue;
            }

            frame.id = msg.identifier;
            frame.dlc = (msg.data_length_code <= 8) ? msg.data_length_code : 8;
            frame.extended = msg.extd;
            frame.remote = msg.rtr;
            memset(frame.data, 0, 8);
            memcpy(frame.data, msg.data, frame.dlc);
            return true;
        }

        return false;
    }

    bool send(const CanFrame &frame) override
    {
        return sendWithPolicy(frame, false);
    }

    bool sendCritical(const CanFrame &frame) override
    {
        return sendWithPolicy(frame, true);
    }

    Diagnostics diagnostics() const override
    {
        return diagnostics_;
    }

private:
    static constexpr uint8_t kMaxExactFilters = 32;
    static constexpr uint16_t kReadDrainBudget = TWAI_READ_DRAIN_BUDGET;
    static constexpr uint32_t BUSOFF_COOLDOWN_MS = 1000;

    static uint8_t popcount11(uint32_t value)
    {
        value &= 0x7FF;
        uint8_t count = 0;
        while (value)
        {
            count += static_cast<uint8_t>(value & 1U);
            value >>= 1;
        }
        return count;
    }

    void configureMaskFilter(twai_filter_config_t &filter, const uint32_t *ids, uint8_t count)
    {
        uint32_t differ = 0;
        for (uint8_t i = 1; i < count; i++)
            differ |= ids[0] ^ ids[i];

        uint32_t base = ids[0] & ~differ;
        filter.acceptance_code = base << 21;
        filter.acceptance_mask = (differ << 21) | 0x001FFFFF;
        filter.single_filter = true;
        diagnostics_.hardwareAcceptedIds = 1UL << popcount11(differ);
    }

    void recordLoopGap()
    {
#ifdef ESP_PLATFORM
        uint32_t now = static_cast<uint32_t>(esp_timer_get_time());
#else
        uint32_t now = millis() * 1000UL;
#endif
        if (lastReadAtUs_ != 0)
        {
            uint32_t gap = now - lastReadAtUs_;
            diagnostics_.lastLoopGapUs = gap;
            if (gap > diagnostics_.maxLoopGapUs)
                diagnostics_.maxLoopGapUs = gap;
        }
        lastReadAtUs_ = now;
    }

    void serviceAlerts()
    {
        if (!driverInstalled_)
            return;
        uint32_t alerts = 0;
        if (twai_read_alerts(&alerts, 0) != ESP_OK)
            return;
        if (alerts & TWAI_ALERT_RX_QUEUE_FULL)
            diagnostics_.rxQueueFull++;
        if (alerts & TWAI_ALERT_RX_FIFO_OVERRUN)
            diagnostics_.rxFifoOverrun++;
        if (alerts & TWAI_ALERT_TX_FAILED)
            diagnostics_.txFailed++;
        if (alerts & TWAI_ALERT_BUS_OFF)
        {
            diagnostics_.busOff++;
            recoverWithCooldown();
        }
        if (alerts & TWAI_ALERT_BUS_RECOVERED)
        {
            diagnostics_.recoverCount++;
            lock();
            driverOK_ = twai_start() == ESP_OK;
            unlock();
        }
    }

    bool sendWithPolicy(const CanFrame &frame, bool critical)
    {
        serviceAlerts();
        lock();
        if (!driverOK_)
        {
            unlock();
            if (onSendFrame)
                onSendFrame(frame, false);
            return false;
        }

        twai_message_t msg = {};
        uint8_t dlc = (frame.dlc <= 8) ? frame.dlc : 8;
        msg.identifier = frame.id;
        msg.data_length_code = dlc;
        memcpy(msg.data, frame.data, dlc);

        bool ok = twai_transmit(&msg, pdMS_TO_TICKS(2)) == ESP_OK;
        if (!ok && critical)
        {
            diagnostics_.txRetry++;
            vTaskDelay(pdMS_TO_TICKS(1));
            ok = twai_transmit(&msg, pdMS_TO_TICKS(2)) == ESP_OK;
        }
        if (!ok)
        {
            diagnostics_.txFailed++;
            if (isBusOff())
                recoverWithCooldownLocked();
        }
        unlock();
        if (onSendFrame)
            onSendFrame(frame, ok);
        return ok;
    }

    bool exactFilterMatchesLocked(uint32_t id) const
    {
        if (exactFilterCount_ == 0)
            return true;
        for (uint8_t i = 0; i < exactFilterCount_; i++)
        {
            if (exactFilterIds_[i] == id)
                return true;
        }
        return false;
    }

    bool isBusOff()
    {
        if (!driverInstalled_)
            return false;
        twai_status_info_t status;
        if (twai_get_status_info(&status) != ESP_OK)
            return false;
        return status.state == TWAI_STATE_BUS_OFF;
    }

    void recoverWithCooldown()
    {
        uint32_t now = millis();
        if (now - lastRecovery_ < BUSOFF_COOLDOWN_MS)
            return;
        lastRecovery_ = now;

        lock();
        initiateRecoveryLocked();
        unlock();
    }

    void recoverWithCooldownLocked()
    {
        uint32_t now = millis();
        if (now - lastRecovery_ < BUSOFF_COOLDOWN_MS)
            return;
        lastRecovery_ = now;
        initiateRecoveryLocked();
    }

    void tryRecover()
    {
        uint32_t now = millis();
        if (now - lastRecovery_ < BUSOFF_COOLDOWN_MS * 10)
            return;
        lastRecovery_ = now;

        diagnostics_.recoverCount++;
        stopAndUninstallLocked();
        driverOK_ = installAndStartLocked();
    }

    void initiateRecoveryLocked()
    {
        if (!driverInstalled_)
            return;
        diagnostics_.recoverCount++;
        driverOK_ = false;
        if (twai_initiate_recovery() != ESP_OK)
        {
            stopAndUninstallLocked();
            driverOK_ = installAndStartLocked();
        }
    }

    void lock()
    {
        if (mutex_)
            xSemaphoreTake(mutex_, portMAX_DELAY);
    }

    void unlock()
    {
        if (mutex_)
            xSemaphoreGive(mutex_);
    }

    bool installAndStartLocked()
    {
        if (twai_driver_install(&g_config_, &t_config_, &f_config_) != ESP_OK)
        {
            driverInstalled_ = false;
            return false;
        }
        driverInstalled_ = true;
        twai_reconfigure_alerts(TWAI_ALERT_BUS_OFF |
                                    TWAI_ALERT_BUS_RECOVERED |
                                    TWAI_ALERT_RECOVERY_IN_PROGRESS |
                                    TWAI_ALERT_ERR_PASS |
                                    TWAI_ALERT_BUS_ERROR |
                                    TWAI_ALERT_TX_FAILED |
                                    TWAI_ALERT_RX_QUEUE_FULL |
                                    TWAI_ALERT_RX_FIFO_OVERRUN,
                                nullptr);
        if (twai_start() != ESP_OK)
        {
            twai_driver_uninstall();
            driverInstalled_ = false;
            return false;
        }
        return true;
    }

    void stopAndUninstallLocked()
    {
        if (!driverInstalled_)
            return;
        twai_stop();
        twai_driver_uninstall();
        driverInstalled_ = false;
        driverOK_ = false;
    }

    gpio_num_t txPin_;
    gpio_num_t rxPin_;
    twai_general_config_t g_config_;
    twai_timing_config_t t_config_;
    twai_filter_config_t f_config_;
    SemaphoreHandle_t mutex_ = nullptr;
    bool driverInstalled_ = false;
    bool driverOK_ = false;
    uint32_t lastRecovery_ = 0;
    uint32_t lastReadAtUs_ = 0;
    uint32_t exactFilterIds_[kMaxExactFilters] = {};
    uint8_t exactFilterCount_ = 0;
    Diagnostics diagnostics_ = {};
};
