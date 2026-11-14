/* Battery level, ADC1 GPIO9, divider 200k / 200k.
 * Only three icons exist (bat20 / bat50 / bat100), so this
 * returns 20, 50 or 100, not a precise percent. Thresholds
 * are on VBAT, not on the pin: pin = VBAT / 2.
 *   >= 3.90 V -> 100
 *   >= 3.60 V -> 50
 *   <  3.60 V -> 20 (lowest icon, also used below 3.2 V;
 *                  there is no bat0 icon in this UI)
 * ADC1 is used, so WiFi does not block it (ADC2 would).
 * Calibration is curve-fitting (IDF v6.1 API) when the
 * chip supports it;
 * without it the fallback is raw * 3300 / 4095, which is
 * still good enough for these wide 300 mV bands.
 */

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_log.h"
#include "gpio_conf.h"
#include "hw_port.h"

static const char *TAG = "hw_port_battery";
static adc_oneshot_unit_handle_t s_unit = NULL;
static adc_cali_handle_t s_cali = NULL;
static bool s_cali_valid = false;
static bool s_inited = false;

esp_err_t hw_port_battery_init(void)
{
    esp_err_t err;
    if (s_inited) return ESP_OK;
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    err = adc_oneshot_new_unit(&unit_cfg, &s_unit);
    if (err != ESP_OK) return err;
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    err = adc_oneshot_config_channel(s_unit, BOARD_BATT_ADC_CHANNEL, &chan_cfg);
    if (err != ESP_OK) return err;
    /* IDF v6.1: line-fitting scheme does not exist in this
     * toolchain (compiler suggests curve-fitting instead),
     * so use curve-fitting. It is per-channel, hence the
     * extra .chan field, and raw_to_voltage() is unchanged. */
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = BOARD_BATT_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) == ESP_OK) {
        s_cali_valid = true;
    } else {
        ESP_LOGW(TAG, "curve-fitting cali unavailable, using raw scale");
    }
    s_inited = true;
    return ESP_OK;
}

esp_err_t hw_port_battery_read_mv(int *vbat_mv)
{
    esp_err_t err;
    int raw = 0;
    int pin_mv = 0;
    if (vbat_mv == NULL) return ESP_ERR_INVALID_ARG;
    err = hw_port_battery_init();
    if (err != ESP_OK) return err;
    err = adc_oneshot_read(s_unit, BOARD_BATT_ADC_CHANNEL, &raw);
    if (err != ESP_OK) return err;
    if (s_cali_valid) {
        err = adc_cali_raw_to_voltage(s_cali, raw, &pin_mv);
        if (err != ESP_OK) return err;
    } else {
        pin_mv = raw * 3300 / 4095;
    }
    *vbat_mv = pin_mv * (BOARD_BATT_DIVIDER_R_TOP_OHM + BOARD_BATT_DIVIDER_R_BOTTOM_OHM)
               / BOARD_BATT_DIVIDER_R_BOTTOM_OHM;
    return ESP_OK;
}

int hw_port_battery_level(void)
{
    int mv = 0;
    if (hw_port_battery_read_mv(&mv) != ESP_OK) return 20;
    if (mv >= 3900) return 100;
    if (mv >= 3600) return 50;
    return 20;
}
