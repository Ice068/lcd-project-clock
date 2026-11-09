#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "driver/i2c.h"
#include "driver/gpio.h"

// --- CONFIG: ตั้งค่า Wi-Fi ของคุณ ---
#define WIFI_SSID      "Ice" //แก้ตรงนี้นะ
#define WIFI_PASS      "Ferrari SF75" // รหัสผ่าน Wi-Fi ของคุณ

// --- การตั้งค่าขา I2C และ LCD 1602 ---
#define I2C_MASTER_NUM I2C_NUM_0
#define I2C_MASTER_SDA_IO 21
#define I2C_MASTER_SCL_IO 22
#define I2C_MASTER_FREQ_HZ 100000
#define LCD_ADDR 0x27  

// --- สวิตช์และ Buzzer ---
#define BTN_MODE   GPIO_NUM_18  
#define BTN_UP     GPIO_NUM_19  
#define BTN_SYNC   GPIO_NUM_23  
#define BUZZER_PIN GPIO_NUM_4   

// Wi-Fi Event Group
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
static const char *TAG = "smart_clock";

typedef enum {
    MODE_TIME = 0,
    MODE_DATE,
    MODE_ALARM_VIEW,
    SET_HOUR,
    SET_MIN,
    SET_DAY,
    SET_MONTH,
    SET_YEAR,
    SET_ALARM_HOUR,
    SET_ALARM_MIN,
    SET_ALARM_STATE
} ClockState;

static ClockState current_state = MODE_TIME;
static bool is_syncing = false;

// [แก้ไข] จะเขียนเวลาลงนาฬิกาก็ต่อเมื่อผู้ใช้แก้ชั่วโมง/นาที/วัน/เดือน/ปี จริง ๆ เท่านั้น
static bool time_edited = false;

// [แก้ไข] flag บอกว่าได้รับเวลาจากเซิร์ฟเวอร์ NTP จริง ๆ แล้ว
static volatile bool s_sntp_synced = false;

// ตัวแปรเวลา
int temp_hour = 12;
int temp_min = 0;
int temp_sec = 0;
int temp_day = 1;
int temp_mon = 1;
int temp_year = 2026;

int alarm_hour = 7;
int alarm_min = 0;
bool alarm_enabled = false;
bool is_alarming = false;

// ป้องกันปลุกซ้ำในนาทีและวันเดิม
static int last_alarm_min = -1;
static int last_alarm_yday = -1;

// [แก้ไข] callback ที่ถูกเรียกเมื่อได้รับเวลาจาก NTP
static void time_sync_cb(struct timeval *tv) {
    struct tm t;
    time_t s = tv->tv_sec;
    localtime_r(&s, &t);
    ESP_LOGI(TAG, "NTP synced: %04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
    s_sntp_synced = true;
}

int get_days_in_month(int month, int year) {
    switch (month) {
        case 2:
            return ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)) ? 29 : 28;
        case 4: case 6: case 9: case 11:
            return 30;
        default:
            return 31;
    }
}

esp_err_t lcd_send_byte(uint8_t val, uint8_t mode) {
    uint8_t high_nibble = val & 0xF0;
    uint8_t low_nibble = (val << 4) & 0xF0;
    uint8_t data[4];
    uint8_t backlight = 0x08; 
    
    data[0] = high_nibble | mode | 0x04 | backlight;
    data[1] = high_nibble | mode | 0x00 | backlight;
    data[2] = low_nibble | mode | 0x04 | backlight;
    data[3] = low_nibble | mode | 0x00 | backlight;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (LCD_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd, data, 4, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);
    vTaskDelay(pdMS_TO_TICKS(2));
    return ret;
}

void lcd_send_cmd(uint8_t cmd) { lcd_send_byte(cmd, 0x00); }
void lcd_send_data(uint8_t data) { lcd_send_byte(data, 0x01); }

void lcd_init() {
    vTaskDelay(pdMS_TO_TICKS(50));
    lcd_send_cmd(0x33); vTaskDelay(pdMS_TO_TICKS(5));
    lcd_send_cmd(0x32); vTaskDelay(pdMS_TO_TICKS(5));
    lcd_send_cmd(0x28); vTaskDelay(pdMS_TO_TICKS(1));
    lcd_send_cmd(0x0C); vTaskDelay(pdMS_TO_TICKS(1));
    lcd_send_cmd(0x06); vTaskDelay(pdMS_TO_TICKS(1));
    lcd_send_cmd(0x01); vTaskDelay(pdMS_TO_TICKS(10));
}

void lcd_set_cursor(uint8_t row, uint8_t col) {
    uint8_t row_offsets[] = {0x00, 0x40};
    lcd_send_cmd(0x80 | (row_offsets[row] + col));
}

void lcd_print_line(uint8_t row, const char *str) {
    char buf[17];
    snprintf(buf, sizeof(buf), "%-16s", str);
    lcd_set_cursor(row, 0);
    char *p = buf;
    while (*p) { 
        lcd_send_data(*p++); 
    }
}

static void i2c_master_init(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    i2c_param_config(I2C_MASTER_NUM, &conf);
    i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
}

void hardware_init(void) {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BTN_MODE) | (1ULL << BTN_UP) | (1ULL << BTN_SYNC),
        .pull_up_en = 1,
        .pull_down_en = 0,
    };
    gpio_config(&io_conf);

    gpio_config_t buzzer_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << BUZZER_PIN),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&buzzer_conf);
    gpio_set_level(BUZZER_PIN, 0);
}

// [แก้ไข] ให้วินาทีเริ่มที่ 0 ตอนกดบันทึก แทนที่จะใช้ค่าวินาทีเก่าที่จดไว้ตอนเข้าโหมดตั้งเวลา
void apply_manual_time() {
    struct tm t = {0};
    t.tm_hour = temp_hour;
    t.tm_min = temp_min;
    t.tm_sec = 0;
    t.tm_mday = temp_day;
    t.tm_mon = temp_mon - 1;
    t.tm_year = temp_year - 1900;
    t.tm_isdst = 0;
    
    time_t new_time = mktime(&t);
    struct timeval tv = { .tv_sec = new_time };
    settimeofday(&tv, NULL);
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WiFi disconnected.");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Got IP address");
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void sync_time_via_wifi() {
    is_syncing = true;
    lcd_print_line(0, " Connecting WiFi");
    lcd_print_line(1, " Please Wait... ");

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));

    if (bits & WIFI_CONNECTED_BIT) {
        lcd_print_line(0, " Syncing NTP... ");
        lcd_print_line(1, " Please Wait... ");

        // [แก้ไข] รอ callback จาก NTP จริง ๆ แทนการเช็กปีของเวลาปัจจุบัน
        s_sntp_synced = false;
        if (esp_sntp_enabled()) {
            esp_sntp_stop();
        }
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_setservername(1, "time.google.com");   // เซิร์ฟเวอร์สำรอง
        sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);       // ตั้งเวลาทันทีที่ได้รับ
        sntp_set_time_sync_notification_cb(time_sync_cb);
        esp_sntp_init();

        int timeout = 150;  // 150 x 100ms = 15 วินาที
        while (!s_sntp_synced && timeout > 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            timeout--;
        }

        esp_sntp_stop();

        if (s_sntp_synced) {
            lcd_print_line(0, "  Time Synced!  ");
            lcd_print_line(1, "   Successful   ");
        } else {
            lcd_print_line(0, "  NTP Timeout!  ");
            lcd_print_line(1, "  Check Server  ");
        }
    } else {
        lcd_print_line(0, "  Sync Failed!  ");
        lcd_print_line(1, " Check WiFiCfg  ");
    }

    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(2000));
    is_syncing = false;
}

bool check_button_pressed(gpio_num_t pin) {
    if (gpio_get_level(pin) == 0) {
        vTaskDelay(pdMS_TO_TICKS(30)); 
        if (gpio_get_level(pin) == 0) {
            return true;
        }
    }
    return false;
}

void clock_task(void *pvParameters) {
    char line0[17];
    char line1[17];

    while (1) {
        if (!is_syncing) {
            time_t now = 0;
            struct tm timeinfo = { 0 };
            time(&now);
            localtime_r(&now, &timeinfo);

            if (alarm_enabled && !is_alarming) {
                if (timeinfo.tm_hour == alarm_hour && timeinfo.tm_min == alarm_min) {
                    if (last_alarm_min != timeinfo.tm_min || last_alarm_yday != timeinfo.tm_yday) {
                        is_alarming = true;
                        last_alarm_min = timeinfo.tm_min;
                        last_alarm_yday = timeinfo.tm_yday;
                    }
                }
            }

            if (is_alarming) {
                gpio_set_level(BUZZER_PIN, 1);
                vTaskDelay(pdMS_TO_TICKS(200));
                gpio_set_level(BUZZER_PIN, 0);
                vTaskDelay(pdMS_TO_TICKS(200));

                if (check_button_pressed(BTN_MODE) || check_button_pressed(BTN_UP) || check_button_pressed(BTN_SYNC)) {
                    is_alarming = false;
                    gpio_set_level(BUZZER_PIN, 0);
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
            } else {
                if (gpio_get_level(BTN_SYNC) == 0) {
                    int press_duration = 0;
                    while (gpio_get_level(BTN_SYNC) == 0) {
                        vTaskDelay(pdMS_TO_TICKS(100));
                        press_duration += 100;
                        if (press_duration >= 1500) break;
                    }

                    if (press_duration >= 1500) {
                        if (current_state == MODE_TIME || current_state == MODE_DATE || current_state == MODE_ALARM_VIEW) {
                            temp_hour = timeinfo.tm_hour;
                            temp_min = timeinfo.tm_min;
                            temp_sec = timeinfo.tm_sec;
                            temp_day = timeinfo.tm_mday;
                            temp_mon = timeinfo.tm_mon + 1;
                            temp_year = timeinfo.tm_year + 1900;
                            time_edited = false;
                            current_state = SET_HOUR; 
                        } else {
                            if (time_edited) { apply_manual_time(); time_edited = false; }
                            current_state = MODE_TIME;
                        }
                    } else {
                        if (current_state == MODE_TIME || current_state == MODE_DATE || current_state == MODE_ALARM_VIEW) {
                            sync_time_via_wifi();
                        }
                    }
                    while (gpio_get_level(BTN_SYNC) == 0) { vTaskDelay(pdMS_TO_TICKS(10)); }
                }

                if (check_button_pressed(BTN_MODE)) {
                    if (current_state == MODE_TIME) {
                        current_state = MODE_DATE;
                    } else if (current_state == MODE_DATE) {
                        current_state = MODE_ALARM_VIEW;
                    } else if (current_state == MODE_ALARM_VIEW) {
                        current_state = MODE_TIME;
                    } else if (current_state == SET_HOUR)       { current_state = SET_MIN; }
                      else if (current_state == SET_MIN)        { current_state = SET_DAY; }
                      else if (current_state == SET_DAY)        { current_state = SET_MONTH; }
                      else if (current_state == SET_MONTH)      { current_state = SET_YEAR; }
                      else if (current_state == SET_YEAR)       {
                          // [แก้ไข] บันทึกเวลาตอนผ่านหน้าตั้งปี ก่อนไปตั้งนาฬิกาปลุก
                          if (time_edited) { apply_manual_time(); time_edited = false; }
                          current_state = SET_ALARM_HOUR;
                      }
                      else if (current_state == SET_ALARM_HOUR) { current_state = SET_ALARM_MIN; }
                      else if (current_state == SET_ALARM_MIN)  { current_state = SET_ALARM_STATE; }
                      else if (current_state == SET_ALARM_STATE){
                          current_state = MODE_TIME; 
                      }
                    while (gpio_get_level(BTN_MODE) == 0) { vTaskDelay(pdMS_TO_TICKS(10)); }
                }

                if (check_button_pressed(BTN_UP)) {
                    if (current_state >= SET_HOUR && current_state <= SET_YEAR) time_edited = true;
                    if (current_state == SET_HOUR) {
                        temp_hour = (temp_hour + 1) % 24;
                    } else if (current_state == SET_MIN) {
                        temp_min = (temp_min + 1) % 60;
                    } else if (current_state == SET_DAY) {
                        int max_days = get_days_in_month(temp_mon, temp_year);
                        temp_day = (temp_day % max_days) + 1;
                    } else if (current_state == SET_MONTH) {
                        temp_mon = (temp_mon % 12) + 1;
                        int max_days = get_days_in_month(temp_mon, temp_year);
                        if (temp_day > max_days) temp_day = max_days;
                    } else if (current_state == SET_YEAR) {
                        temp_year++;
                        if (temp_year > 2040) temp_year = 2026;
                    } else if (current_state == SET_ALARM_HOUR) {
                        alarm_hour = (alarm_hour + 1) % 24;
                    } else if (current_state == SET_ALARM_MIN) {
                        alarm_min = (alarm_min + 1) % 60;
                    } else if (current_state == SET_ALARM_STATE) {
                        alarm_enabled = !alarm_enabled;
                    }
                    while (gpio_get_level(BTN_UP) == 0) { vTaskDelay(pdMS_TO_TICKS(10)); }
                }
            }

            #pragma GCC diagnostic push
            #pragma GCC diagnostic ignored "-Wformat-truncation"
            if (is_alarming) {
                snprintf(line0, sizeof(line0), "  *** ALARM! ***");
                snprintf(line1, sizeof(line1), "  WAKE UP NOW!  ");
            } else if (current_state == MODE_TIME) {
                snprintf(line0, sizeof(line0), "  Smart Clock   ");
                snprintf(line1, sizeof(line1), "    %02d:%02d:%02d    ", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
            } else if (current_state == MODE_DATE) {
                snprintf(line0, sizeof(line0), "  Current Date  ");
                snprintf(line1, sizeof(line1), "   %02d/%02d/%04d   ", timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900);
            } else if (current_state == MODE_ALARM_VIEW) {
                snprintf(line0, sizeof(line0), "  Alarm Status  ");
                snprintf(line1, sizeof(line1), "   %02d:%02d [%s]   ", alarm_hour, alarm_min, alarm_enabled ? "ON" : "OFF");
            } else if (current_state == SET_HOUR) {
                snprintf(line0, sizeof(line0), ">> SET HOUR <<  ");
                snprintf(line1, sizeof(line1), "     [ %02d ]     ", temp_hour);
            } else if (current_state == SET_MIN) {
                snprintf(line0, sizeof(line0), ">> SET MINUTE <<");
                snprintf(line1, sizeof(line1), "     [ %02d ]     ", temp_min);
            } else if (current_state == SET_DAY) {
                snprintf(line0, sizeof(line0), ">> SET DAY <<   ");
                snprintf(line1, sizeof(line1), "     [ %02d ]     ", temp_day);
            } else if (current_state == SET_MONTH) {
                snprintf(line0, sizeof(line0), ">> SET MONTH << ");
                snprintf(line1, sizeof(line1), "     [ %02d ]     ", temp_mon);
            } else if (current_state == SET_YEAR) {
                snprintf(line0, sizeof(line0), ">> SET YEAR <<  ");
                snprintf(line1, sizeof(line1), "     [ %04d ]   ", temp_year);
            } else if (current_state == SET_ALARM_HOUR) {
                snprintf(line0, sizeof(line0), ">> ALARM HOUR <<");
                snprintf(line1, sizeof(line1), "     [ %02d ]     ", alarm_hour);
            } else if (current_state == SET_ALARM_MIN) {
                snprintf(line0, sizeof(line0), ">> ALARM MIN  <<");
                snprintf(line1, sizeof(line1), "     [ %02d ]     ", alarm_min);
            } else if (current_state == SET_ALARM_STATE) {
                snprintf(line0, sizeof(line0), ">> ALARM ON/OFF");
                snprintf(line1, sizeof(line1), "     [ %s ]     ", alarm_enabled ? "ENABLE" : "DISABLE");
            }
            #pragma GCC diagnostic pop

            lcd_print_line(0, line0);
            lcd_print_line(1, line1);
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    setenv("TZ", "ICT-7", 1);
    tzset();

    i2c_master_init();
    lcd_init();
    hardware_init();

    xTaskCreate(clock_task, "clock_task", 4096, NULL, 5, NULL);
}