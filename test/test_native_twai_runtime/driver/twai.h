#pragma once
#include <cstdint>
#include <vector>
using esp_err_t=int; using gpio_num_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_TIMEOUT=1,ESP_ERR_INVALID_STATE=2,TWAI_MODE_NORMAL=0;
enum {TWAI_STATE_STOPPED,TWAI_STATE_RUNNING,TWAI_STATE_BUS_OFF,TWAI_STATE_RECOVERING};
enum {TWAI_ALERT_RX_QUEUE_FULL=1,TWAI_ALERT_RX_FIFO_OVERRUN=2,TWAI_ALERT_TX_FAILED=4,TWAI_ALERT_BUS_OFF=8,
      TWAI_ALERT_BUS_RECOVERED=16,TWAI_ALERT_RECOVERY_IN_PROGRESS=32,TWAI_ALERT_ERR_PASS=64,TWAI_ALERT_BUS_ERROR=128};
struct twai_general_config_t { uint32_t rx_queue_len=0,tx_queue_len=0; };
struct twai_timing_config_t {};
struct twai_filter_config_t {uint32_t acceptance_code=0,acceptance_mask=UINT32_MAX;bool single_filter=true;};
struct twai_message_t {uint32_t identifier=0;uint8_t data_length_code=8,data[8]{};bool extd=false,rtr=false;};
struct twai_status_info_t {int state=TWAI_STATE_RUNNING;uint32_t msgs_to_tx=0;};
#define TWAI_GENERAL_CONFIG_DEFAULT(tx,rx,mode) twai_general_config_t{}
#define TWAI_TIMING_CONFIG_500KBITS() twai_timing_config_t{}
#define TWAI_FILTER_CONFIG_ACCEPT_ALL() twai_filter_config_t{}
inline twai_status_info_t fakeStatus;
inline uint32_t fakeAlerts=0,fakeInstalls=0,fakeTxWait=99,fakeTransmits=0,fakeRxQueue=0,fakeTxQueue=99;
inline int fakeTxError=ESP_OK;
inline std::vector<twai_message_t> fakeRx;
inline int twai_driver_install(const twai_general_config_t *g,const twai_timing_config_t *,const twai_filter_config_t *) {
    ++fakeInstalls;fakeRxQueue=g->rx_queue_len;fakeTxQueue=g->tx_queue_len;fakeStatus.state=TWAI_STATE_STOPPED;return ESP_OK;
}
inline int twai_start(){fakeStatus.state=TWAI_STATE_RUNNING;fakeStatus.msgs_to_tx=0;return ESP_OK;}
inline int twai_stop(){fakeStatus.state=TWAI_STATE_STOPPED;fakeStatus.msgs_to_tx=0;return ESP_OK;}
inline int twai_driver_uninstall(){return ESP_OK;}
inline int twai_reconfigure_alerts(uint32_t,uint32_t*){return ESP_OK;}
inline int twai_read_alerts(uint32_t *a,uint32_t){*a=fakeAlerts;fakeAlerts=0;return ESP_OK;}
inline int twai_receive(twai_message_t *m,uint32_t){if(fakeRx.empty())return ESP_ERR_TIMEOUT;*m=fakeRx.front();fakeRx.erase(fakeRx.begin());return ESP_OK;}
inline int twai_get_status_info(twai_status_info_t *s){*s=fakeStatus;return ESP_OK;}
inline int twai_initiate_recovery(){fakeStatus.state=TWAI_STATE_RECOVERING;return ESP_OK;}
inline int twai_transmit(const twai_message_t *,uint32_t wait){++fakeTransmits;fakeTxWait=wait;if(fakeTxError)return fakeTxError;fakeStatus.msgs_to_tx=1;return ESP_OK;}
