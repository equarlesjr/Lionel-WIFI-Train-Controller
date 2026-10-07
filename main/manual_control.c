#include "manual_control.h"

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "train_control.h"

static const char *TAG = "manual_control";

#define MANUAL_POT_GPIO           (4) /* XIAO ESP32-S3 D3 */
#define MANUAL_WEB_GPIO           (5) /* XIAO ESP32-S3 D4 */
#define MANUAL_KNOB_GPIO          (6) /* XIAO ESP32-S3 D5 */
#define MANUAL_SAMPLE_MS          (50)
#define MANUAL_DEBOUNCE_SAMPLES   (3)
#define MANUAL_AVG_SAMPLES        (8)
#define MANUAL_HYST_PERCENT       (2)
#define MANUAL_ADC_RAW_MAX        (4095)
#define MANUAL_TASK_STACK         (4096)
#define MANUAL_TASK_PRIORITY      (5)
#define MANUAL_DEBUG_LOG_MS       (500)

static const char *const KNOB_MODES[] = {"off", "1", "2", "3", "4", "5", "6"};

static adc_oneshot_unit_handle_t s_adc;
static adc_channel_t s_adc_channel;
static bool s_adc_ok;
static bool s_knob_armed;
static int s_knob_level;
static int s_avg_samples[MANUAL_AVG_SAMPLES];
static int s_avg_sum;
static int s_avg_index;
static int s_avg_count;
static int s_debounce_web = 1;
static int s_debounce_knob = 1;
static int s_pending_web = 1;
static int s_pending_knob = 1;
static int s_pending_count;
static train_control_source_t s_applied_source = TRAIN_SOURCE_OFF;
static bool s_adc_fail_logged;
static int s_last_logged_web = -1;
static int s_last_logged_knob = -1;
static int s_last_logged_pct = -1;
static TickType_t s_last_status_log;

static void reset_smoother(void)
{
    s_avg_sum = 0;
    s_avg_index = 0;
    s_avg_count = 0;
}

static int smooth_raw(int raw)
{
    if (s_avg_count == MANUAL_AVG_SAMPLES) {
        s_avg_sum -= s_avg_samples[s_avg_index];
    } else {
        s_avg_count++;
    }

    s_avg_samples[s_avg_index] = raw;
    s_avg_sum += raw;
    s_avg_index = (s_avg_index + 1) % MANUAL_AVG_SAMPLES;
    return s_avg_sum / s_avg_count;
}

static unsigned raw_to_percent(int raw)
{
    if (raw < 0) {
        raw = 0;
    }
    if (raw > MANUAL_ADC_RAW_MAX) {
        raw = MANUAL_ADC_RAW_MAX;
    }
    return (unsigned)((raw * 100 + MANUAL_ADC_RAW_MAX / 2) / MANUAL_ADC_RAW_MAX);
}

static int map_percent_to_level(unsigned pct)
{
    if (pct < 5) {
        return 0;
    }
    if (pct < 20) {
        return 1;
    }
    if (pct < 35) {
        return 2;
    }
    if (pct < 50) {
        return 3;
    }
    if (pct < 65) {
        return 4;
    }
    if (pct < 80) {
        return 5;
    }
    return 6;
}

static int apply_hysteresis(unsigned pct, int current)
{
    int candidate = map_percent_to_level(pct);
    if (candidate == current) {
        return current;
    }

    if (candidate > current) {
        unsigned enter;
        switch (candidate) {
        case 1: enter = 5 + MANUAL_HYST_PERCENT; break;
        case 2: enter = 20 + MANUAL_HYST_PERCENT; break;
        case 3: enter = 35 + MANUAL_HYST_PERCENT; break;
        case 4: enter = 50 + MANUAL_HYST_PERCENT; break;
        case 5: enter = 65 + MANUAL_HYST_PERCENT; break;
        default: enter = 80 + MANUAL_HYST_PERCENT; break;
        }
        return (pct >= enter) ? candidate : current;
    }

    unsigned leave;
    switch (current) {
    case 1: leave = 5; break;
    case 2: leave = 20; break;
    case 3: leave = 35; break;
    case 4: leave = 50; break;
    case 5: leave = 65; break;
    default: leave = 80; break;
    }
    if (leave > MANUAL_HYST_PERCENT) {
        leave -= MANUAL_HYST_PERCENT;
    } else {
        leave = 0;
    }
    return (pct < leave) ? candidate : current;
}

static const char *source_to_label(train_control_source_t source)
{
    switch (source) {
    case TRAIN_SOURCE_WEB:
        return "WEB";
    case TRAIN_SOURCE_KNOB:
        return "KNOB";
    case TRAIN_SOURCE_OFF:
    default:
        return "OFF";
    }
}

static const char *pins_to_label(int web_level, int knob_level)
{
    if (web_level == 0 && knob_level == 1) {
        return "WEB";
    }
    if (web_level == 1 && knob_level == 1) {
        return "OFF";
    }
    if (web_level == 1 && knob_level == 0) {
        return "KNOB";
    }
    return "INVALID";
}

static train_control_source_t source_from_pins(int web_level, int knob_level)
{
    const bool web_selected = (web_level == 0);
    const bool knob_selected = (knob_level == 0);

    if (web_selected && !knob_selected) {
        return TRAIN_SOURCE_WEB;
    }
    if (!web_selected && knob_selected) {
        return TRAIN_SOURCE_KNOB;
    }
    return TRAIN_SOURCE_OFF;
}

static void log_hw_status(int web_level, int knob_level, int adc_raw, int adc_smooth,
                          unsigned pct, bool adc_ok, int mapped_level)
{
    ESP_LOGI(TAG,
             "SWITCH GPIO5/WEB=%d GPIO6/KNOB=%d instant=%s settled=%s | "
             "POT adc=%s raw=%d smooth=%d pct=%u%% maps_to=%s armed=%d",
             web_level, knob_level,
             pins_to_label(web_level, knob_level),
             source_to_label(s_applied_source),
             adc_ok ? "ok" : "FAIL",
             adc_raw, adc_smooth, pct,
             KNOB_MODES[mapped_level],
             (int)s_knob_armed);
}

static train_control_source_t debounce_source(int web_level, int knob_level)
{
    if (web_level == s_pending_web && knob_level == s_pending_knob) {
        if (s_pending_count < MANUAL_DEBOUNCE_SAMPLES) {
            s_pending_count++;
        }
    } else {
        s_pending_web = web_level;
        s_pending_knob = knob_level;
        s_pending_count = 1;
    }

    if (s_pending_count >= MANUAL_DEBOUNCE_SAMPLES) {
        s_debounce_web = s_pending_web;
        s_debounce_knob = s_pending_knob;
    }

    return source_from_pins(s_debounce_web, s_debounce_knob);
}

static void force_knob_off(void)
{
    s_knob_armed = false;
    s_knob_level = 0;
    reset_smoother();
    (void)train_control_apply_knob_mode("off");
}

static void apply_source(train_control_source_t source)
{
    if (source != s_applied_source) {
        ESP_LOGI(TAG, "Selector -> %s", source_to_label(source));
        s_knob_armed = false;
        s_knob_level = 0;
        reset_smoother();
        train_control_set_source(source);
        s_applied_source = source;
        return;
    }

    if (source == TRAIN_SOURCE_OFF) {
        train_control_set_source(TRAIN_SOURCE_OFF);
    }
}

static bool read_pot(int *raw_out, int *smooth_out, unsigned *pct_out)
{
    int raw = 0;

    *raw_out = -1;
    *smooth_out = -1;
    *pct_out = 0;

    if (!s_adc_ok || s_adc == NULL) {
        if (!s_adc_fail_logged) {
            ESP_LOGE(TAG, "ADC unavailable; commanding OFF");
            s_adc_fail_logged = true;
        }
        return false;
    }

    if (adc_oneshot_read(s_adc, s_adc_channel, &raw) != ESP_OK) {
        if (!s_adc_fail_logged) {
            ESP_LOGE(TAG, "ADC read failed; commanding OFF");
            s_adc_fail_logged = true;
        }
        return false;
    }

    s_adc_fail_logged = false;
    int smooth = smooth_raw(raw);
    *raw_out = raw;
    *smooth_out = smooth;
    *pct_out = raw_to_percent(smooth);
    return true;
}

static void apply_knob_from_reading(unsigned pct)
{
    s_knob_level = apply_hysteresis(pct, s_knob_level);

    if (!s_knob_armed) {
        (void)train_control_apply_knob_mode("off");
        if (s_knob_level == 0) {
            s_knob_armed = true;
            ESP_LOGI(TAG, "Knob zero acknowledged; manual throttle armed");
        }
        return;
    }

    (void)train_control_apply_knob_mode(KNOB_MODES[s_knob_level]);
}

static void maybe_log_hw(int web_level, int knob_level, int adc_raw, int adc_smooth,
                         unsigned pct, bool adc_ok)
{
    TickType_t now = xTaskGetTickCount();
    int mapped = adc_ok ? (int)map_percent_to_level(pct) : 0;
    bool pins_changed = (web_level != s_last_logged_web) || (knob_level != s_last_logged_knob);
    bool pot_changed = adc_ok && (s_last_logged_pct < 0 ||
                                  (int)pct >= s_last_logged_pct + 2 ||
                                  (int)pct + 2 <= s_last_logged_pct);
    bool heartbeat = (now - s_last_status_log) >= pdMS_TO_TICKS(MANUAL_DEBUG_LOG_MS);

    if (!pins_changed && !pot_changed && !heartbeat) {
        return;
    }

    log_hw_status(web_level, knob_level, adc_raw, adc_smooth, pct, adc_ok, mapped);

    s_last_logged_web = web_level;
    s_last_logged_knob = knob_level;
    if (adc_ok) {
        s_last_logged_pct = (int)pct;
    }
    s_last_status_log = now;
}

static void manual_control_task(void *arg)
{
    (void)arg;

    while (1) {
        int web_level = gpio_get_level(MANUAL_WEB_GPIO);
        int knob_level = gpio_get_level(MANUAL_KNOB_GPIO);
        train_control_source_t source = debounce_source(web_level, knob_level);

        apply_source(source);

        int adc_raw = -1;
        int adc_smooth = -1;
        unsigned pct = 0;
        bool adc_ok = read_pot(&adc_raw, &adc_smooth, &pct);

        if (s_applied_source == TRAIN_SOURCE_KNOB) {
            if (!adc_ok) {
                force_knob_off();
            } else {
                apply_knob_from_reading(pct);
            }
        }

        maybe_log_hw(web_level, knob_level, adc_raw, adc_smooth, pct, adc_ok);
        vTaskDelay(pdMS_TO_TICKS(MANUAL_SAMPLE_MS));
    }
}

static bool init_selector_gpio(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << MANUAL_WEB_GPIO) | (1ULL << MANUAL_KNOB_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io_conf) != ESP_OK) {
        ESP_LOGE(TAG, "Selector GPIO init failed; treating as OFF");
        return false;
    }
    return true;
}

static bool init_adc(void)
{
    adc_unit_t unit = ADC_UNIT_1;
    adc_channel_t channel = ADC_CHANNEL_0;

    if (adc_oneshot_io_to_channel(MANUAL_POT_GPIO, &unit, &channel) != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d is not an ADC pad", MANUAL_POT_GPIO);
        return false;
    }

    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = unit,
    };
    if (adc_oneshot_new_unit(&init_config, &s_adc) != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit init failed");
        s_adc = NULL;
        return false;
    }

    adc_oneshot_chan_cfg_t chan_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_oneshot_config_channel(s_adc, channel, &chan_config) != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed");
        return false;
    }

    s_adc_channel = channel;
    ESP_LOGI(TAG, "Potentiometer ADC on GPIO%d", MANUAL_POT_GPIO);
    return true;
}

static train_control_source_t establish_selector(void)
{
    train_control_source_t source = TRAIN_SOURCE_OFF;

    for (int i = 0; i < MANUAL_DEBOUNCE_SAMPLES; i++) {
        source = debounce_source(gpio_get_level(MANUAL_WEB_GPIO),
                                 gpio_get_level(MANUAL_KNOB_GPIO));
        vTaskDelay(pdMS_TO_TICKS(MANUAL_SAMPLE_MS));
    }
    return source;
}

void manual_control_init(void)
{
    s_knob_armed = false;
    s_knob_level = 0;
    reset_smoother();

    bool gpio_ok = init_selector_gpio();
    s_adc_ok = init_adc();
    if (!s_adc_ok) {
        ESP_LOGE(TAG, "ADC init failed; knob input disabled, PWM stays OFF until WEB");
    }

    train_control_source_t source = TRAIN_SOURCE_OFF;
    if (gpio_ok) {
        source = establish_selector();
    }

    train_control_set_source(source);
    s_applied_source = source;
    ESP_LOGI(TAG, "Manual control ready; selector=%s, adc_ok=%d",
             source_to_label(source), (int)s_adc_ok);
    ESP_LOGI(TAG,
             "Probe pins: WEB=GPIO%d KNOB=GPIO%d POT=GPIO%d (0=closed to GND on switch)",
             MANUAL_WEB_GPIO, MANUAL_KNOB_GPIO, MANUAL_POT_GPIO);

    xTaskCreate(manual_control_task, "manual_ctrl", MANUAL_TASK_STACK, NULL,
                MANUAL_TASK_PRIORITY, NULL);
}
