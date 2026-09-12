#pragma once
// Included by app.h after its driver/broker globals. Configuration writes are
// serialized at RX transaction boundaries, including the physical toggle button.
static Shared<uint32_t> appConfigEpoch{1};
static Shared<bool> appTxPaused{false};
static Shared<uint32_t> appConfigPending{0};
#if defined(ESP_PLATFORM) && !defined(NATIVE_BUILD)
static StaticSemaphore_t appCanMutexStorage;
static SemaphoreHandle_t appCanMutex=xSemaphoreCreateRecursiveMutexStatic(&appCanMutexStorage);
#endif
struct AppCanLock {
    bool held=true;
    explicit AppCanLock(bool wait=true) {
#if defined(ESP_PLATFORM) && !defined(NATIVE_BUILD)
        held=xSemaphoreTakeRecursive(appCanMutex,wait ? portMAX_DELAY : 0) == pdTRUE;
#else
        (void)wait;
#endif
    }
    void unlock() {
#if defined(ESP_PLATFORM) && !defined(NATIVE_BUILD)
        if (held) xSemaphoreGiveRecursive(appCanMutex);
#endif
        held=false;
    }
    ~AppCanLock() { unlock(); }
};

static bool appQuiesceRuntime()
{
    appTxPaused=true;
    AppCanLock lock;
    ++appConfigEpoch;
    appTxBroker.cancelAll(millis());
    return !appDriver || appDriver->quiesce(20);
}

static void appResumeRuntime()
{
    AppCanLock lock;
    ++appConfigEpoch;
    appTxBroker.cancelAll(millis());
    if (appDriver) appDriver->resume();
    appTxPaused=false;
}

struct AppConfigTransaction {
    struct Pending {
        bool held=true;
        Pending() { ++appConfigPending; }
        void release() { if (held) { held=false; --appConfigPending; } }
        ~Pending() { release(); }
    } pending;
    AppCanLock lock;
    bool ready=true;
    bool finished=false;
    AppConfigTransaction() {
        ++appConfigEpoch;
        appTxBroker.cancelAll(millis());
        if (appDriver) ready=appDriver->quiesce(20);
    }
    void finish() {
        if (finished) return;
        finished=true;
        if (appDriver && !appTxPaused) appDriver->resume();
        pending.release();
        lock.unlock();
    }
    ~AppConfigTransaction() { finish(); }
};
