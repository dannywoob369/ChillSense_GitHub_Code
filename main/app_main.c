/*
 * ChillSense data flow
 *
 * Flow meter
 *    |
 *    | dry contact pulse
 *    v
 * GPIO4 interrupt
 *    |
 *    v
 * Pulse processing task
 *    |
 *    | pulse count / GPM / total gallons
 *    v
 * Telemetry task
 *    |
 *    | JSON every 5 seconds
 *    v
 * MQTT over WiFi/TLS
 *    |
 *    v
 * MQTT broker
 *    |
 *    v
 * MQTTX / future cloud dashboard
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "driver/gpio.h"

#include "esp_adc/adc_oneshot.h"

#include "protocol_examples_common.h"

#include "esp_crt_bundle.h"
#include "mqtt_client.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

//---------------------------------------------------------
//                    Configuration
//---------------------------------------------------------
#define FLOW_GPIO GPIO_NUM_4

//thermistor
#define THERMISTOR_GPIO GPIO_NUM_1
#define THERMISTOR_ADC_CHANNEL ADC_CHANNEL_0

//white thermistor status LED
#define THERMISTOR_LED_GPIO GPIO_NUM_14

//thermistor specifications
#define THERMISTOR_R0_OHMS 50000.0f
#define THERMISTOR_BETA 3590.0f
#define THERMISTOR_T0_C 25.0f

//resistor used in voltage divider
//use 47k or 50k physically on the breadboard
#define THERMISTOR_FIXED_RESISTOR_OHMS 50000.0f

//temp health-check thresholds
#define THERMISTOR_MIN_RAW 100
#define THERMISTOR_MAX_RAW 3900

#define THERMISTOR_CHECK_PERIOD_MS 1000

//data LED
#define DATA_LED_GPIO GPIO_NUM_15

//meter is 1 gallon per pulse (tempoary value since the meter is mcuh faster than 1 gallon per pulse)
#define GALLONS_PER_PULSE 1.0f

//ignore additional switch closures that happen too soon afterthe previous pulse
//100 ms is fine for this slow mechanical/dry-contact meter
#define DEBOUNCE_TIME_US 100000LL

//if no pulse occurs for this long, report flow as zero
//meter nominal minimum is around 1 GPM, which is one pulse every 60 seconds with a 1 gallon per pulse output
#define FLOW_TIMEOUT_US 90000000LL

#define TELEMETRY_PERIOD_MS 5000

static const char *TAG = "chillsense";


//---------------------------------------------------------
//                      MQTT state
//--------------------------------------------------------- 
static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;


//---------------------------------------------------------
//                  Flowmeter State
//--------------------------------------------------------- 
static TaskHandle_t pulse_task_handle = NULL;
static SemaphoreHandle_t meter_mutex = NULL;

static uint32_t total_pulses = 0;
static float flow_gpm = 0.0f;

static int64_t last_valid_pulse_us = 0;
static int64_t previous_valid_pulse_us = 0;

static int64_t last_pulse_interval_us = 0;

//---------------------------------------------------------
//                  Thermistor State
//---------------------------------------------------------
static adc_oneshot_unit_handle_t adc1_handle;

//---------------------------------------------------------
//           MQTT Certificate Configuration
//--------------------------------------------------------- 
#if CONFIG_EXAMPLE_BROKER_CERTIFICATE_OVERRIDDEN
static const char cert_override_pem[] =
    "-----BEGIN CERTIFICATE-----\n"
    CONFIG_EXAMPLE_BROKER_CERTIFICATE_OVERRIDE "\n"
    "-----END CERTIFICATE-----";
#endif

#if CONFIG_EXAMPLE_CERT_VALIDATE_MOSQUITTO_CA
extern const uint8_t mosquitto_org_crt_start[]
    asm("_binary_mosquitto_org_crt_start");
extern const uint8_t mosquitto_org_crt_end[]
    asm("_binary_mosquitto_org_crt_end");
#endif


//---------------------------------------------------------
//                      GPIO Interrupt
//---------------------------------------------------------
//Called automatically whenever the GPIO4 detects a falling edge
//
//In the circuit:
//HIGH = meter contact open
//LOW  = meter contact closed
//
//So HIGH -> LOW means one meter pulse occurred
//
//The ISR does NOT process the pulse directly
//It only wakes the pulse processing task below
static void flow_gpio_isr_handler(void *arg)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    if (pulse_task_handle != NULL) 
    {
        //notify the normal FreeRTOS task that a pulse occurred
        vTaskNotifyGiveFromISR(pulse_task_handle, &higher_priority_task_woken);
    }

    if (higher_priority_task_woken == pdTRUE) 
    {
        //immediately allow the awakened task to run if needed
        portYIELD_FROM_ISR();
    }
}

//---------------------------------------------------------
//              Flow pulse processing task
//--------------------------------------------------------- 
//This task sleeps until the GPIO interrupt reports a pulse
//Once awake:
//1. rejects switch bounce
//2. counts the pulse
//3. calculates total gallons
//4. calculates GPM from the time between pulses
static void pulse_task(void *pvParameters)
{
    int64_t last_interrupt_us = 0;

    while (1) 
    {
        //wait here until the GPIO interrupt wakes
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        //ESP timer returns elapsed time in microseconds
        int64_t now_us = esp_timer_get_time();

        //ignore duplicate electrical transitions that occur within 100 ms of the previous accepted pulse
        if ((now_us - last_interrupt_us) < DEBOUNCE_TIME_US) 
        {
            continue;
        }

        last_interrupt_us = now_us;

        //lock shared flow meter variables so the MQTT task cannot read them while they are being modified
        xSemaphoreTake(meter_mutex, portMAX_DELAY);

        total_pulses++;

        previous_valid_pulse_us = last_valid_pulse_us;
        last_valid_pulse_us = now_us;

        //one pulse alone only tells us volume
        //two pulses are required to calculate flow rate because GPM depends on time between pulses
        if (previous_valid_pulse_us > 0) 
        {
            int64_t pulse_interval_us = last_valid_pulse_us - previous_valid_pulse_us;

            if (pulse_interval_us > 0) 
            {   

                //save the interval for adaptive timeout
                last_pulse_interval_us = pulse_interval_us;

                //flow calculation:
                //1 pulse = 1 gallon
                //
                //general flow formula:
                //GPM = gallons per pulse × 60 seconds/minute / seconds between pulses
                //
                //since our timing is in microseconds, multiply by 60,000,000 instead.
                flow_gpm = (GALLONS_PER_PULSE * 60000000.0f) / (float)pulse_interval_us;
            }
        }

        float total_gallons = total_pulses * GALLONS_PER_PULSE;

        ESP_LOGI(TAG, "Flow pulse detected: pulses=%" PRIu32 ", total=%.2f gal, flow=%.2f GPM", total_pulses, total_gallons, flow_gpm);
        //release the shared data for the MQTT task
        xSemaphoreGive(meter_mutex);
    }
}


//---------------------------------------------------------
//flow GPIO initialization
//--------------------------------------------------------- 
static void flow_meter_init(void)
{
    gpio_config_t flow_gpio_config = 
    {
        .pin_bit_mask = (1ULL << FLOW_GPIO),

        //input only
        .mode = GPIO_MODE_INPUT,

        //enable internal pull-up as an additional safeguard
        //should still use the external 10k pull-up resistor from GPIO4 to 3.3 V
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,

        //meter contact closing pulls GPIO4 LOW
        .intr_type = GPIO_INTR_NEGEDGE
    };

    ESP_ERROR_CHECK(gpio_config(&flow_gpio_config));

    //install GPIO interrupt service
    esp_err_t isr_result = gpio_install_isr_service(0);

    //if "ESP_ERR_INVALID_STATE" appears it means the ISR service was already installed elsewhere
    if (isr_result != ESP_OK && isr_result != ESP_ERR_INVALID_STATE) 
    {
        ESP_ERROR_CHECK(isr_result);
    }

    ESP_ERROR_CHECK(gpio_isr_handler_add(FLOW_GPIO, flow_gpio_isr_handler, NULL));

    ESP_LOGI(TAG, "Flow meter input initialized on GPIO%d", FLOW_GPIO);
}

//---------------------------------------------------------
//              Thermistor initialization
//---------------------------------------------------------
static void thermistor_init(void)
{
    //-----------------------------------------------------
    //           thermistor status LED (white)
    //-----------------------------------------------------
    gpio_config_t led_config =
    {
        .pin_bit_mask = (1ULL << THERMISTOR_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    ESP_ERROR_CHECK(gpio_config(&led_config));

    //LED starts OFF until a valid sensor reading is detected
    gpio_set_level(THERMISTOR_LED_GPIO, 0);


    //-----------------------------------------------------
    //              ADC1 initialization
    //-----------------------------------------------------
    adc_oneshot_unit_init_cfg_t adc_init_config = {.unit_id = ADC_UNIT_1};

    ESP_ERROR_CHECK(adc_oneshot_new_unit(&adc_init_config, &adc1_handle));


    //-----------------------------------------------------
    //          GPIO1 / ADC1 Channel 0 configuration
    //-----------------------------------------------------
    adc_oneshot_chan_cfg_t adc_channel_config = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};

    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, THERMISTOR_ADC_CHANNEL, &adc_channel_config));

    ESP_LOGI(TAG, "Thermistor initialized on GPIO%d, status LED on GPIO%d", THERMISTOR_GPIO, THERMISTOR_LED_GPIO);
}

//---------------------------------------------------------
//          Periodic MQTT telemetry task
//--------------------------------------------------------- 
// runs every 5 seconds (for testing, will be changed to longer intervals in the future)
// this task does:
// 1. reads the latest flow-meter values
// 2. builds a JSON message
// 3. publishes that message to the MQTT broker
static void telemetry_task(void *pvParameters)
{
    TickType_t last_wake_time = xTaskGetTickCount();

    while (1) 
    {
        if (mqtt_connected && mqtt_client != NULL) 
        {
            uint32_t pulses_copy;
            float flow_copy;
            float gallons_copy;
            int64_t last_pulse_copy;
            int64_t interval_copy;

            //copy shared meter values while the mutex is locked.
            //this prevents the pulse task from chaning them halfway through the copy.
            xSemaphoreTake(meter_mutex, portMAX_DELAY);

            pulses_copy = total_pulses;
            flow_copy = flow_gpm;
            gallons_copy = total_pulses * GALLONS_PER_PULSE;
            last_pulse_copy = last_valid_pulse_us;
            interval_copy = last_pulse_interval_us;

            xSemaphoreGive(meter_mutex);

        //determine whether the latest flow measurement is still valid.
        int64_t now_us = esp_timer_get_time();

        if (last_pulse_copy == 0 || interval_copy == 0)
        {
            // Not enough pulses have been received
            // to calculate an actual flow rate.
            flow_copy = 0.0f;
        }
        else
        {
            // Start with the existing maximum timeout.
            int64_t adaptive_timeout_us = FLOW_TIMEOUT_US;

            // Use three times the previous pulse interval,
            // provided it is below the maximum timeout.
            if (interval_copy < FLOW_TIMEOUT_US / 3)
            {
                adaptive_timeout_us = interval_copy * 3;
            }

            // Prevent extremely short timeouts.
            if (adaptive_timeout_us < 3000000LL)
            {
                adaptive_timeout_us = 3000000LL;
            }

            // Check whether the previous measurement is stale.
            if ((now_us - last_pulse_copy) > adaptive_timeout_us)
            {
                flow_copy = 0.0f;
            }
        }

            //temperature values are placeholders for now
            float supply_temp_f = 44.0f;
            float return_temp_f = 55.0f;

            char payload[256];

            //format the sensor data as JSON
            snprintf(payload, sizeof(payload),

                "{"
                "\"device_id\":\"chillsense_test_01\","
                "\"flow_gpm\":%.2f,"
                "\"total_gallons\":%.2f,"
                "\"pulse_count\":%" PRIu32 ","
                "\"supply_temp_f\":%.1f,"
                "\"return_temp_f\":%.1f"
                "}",

                flow_copy, gallons_copy, pulses_copy, supply_temp_f, return_temp_f);

            //publish the JSON message to the MQTT broker
            //Qos 1 means deliver this message at least once
            int msg_id = esp_mqtt_client_publish(mqtt_client, "chillsense/test/telemetry", payload, 0, 1, 0);
            
            if (msg_id >= 0)
            {
                //flash data LED whenever telemetry is sent
                gpio_set_level(DATA_LED_GPIO, 1);
                vTaskDelay(pdMS_TO_TICKS(100));
                gpio_set_level(DATA_LED_GPIO, 0);
            }


            ESP_LOGI(TAG, "Telemetry sent: %s | msg_id=%d", payload, msg_id);
        }
        
        //maintain a consistent telemetry period
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(TELEMETRY_PERIOD_MS));
    }
}

//---------------------------------------------------------
//                  MQTT event handler
//---------------------------------------------------------
// This function is called automatically by the MQTT library whenever something important happens with the MQTT connection.
// examples:
// -connection established
// -connection lost
// -message successfully published
// -incoming message received
// -connection/error condition occurs
// The event_id tells us which MQTT event occurred.
// (this might need some more work depending on how our hardware will be set up.)
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    //debug message showing which MQTT event was received
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32, base, event_id);

    //convert the generic event data into an MQTT specific event structure
    //this gives access to things like: message ID, topic, payload, and error information
    esp_mqtt_event_handle_t event = event_data;

    //how the code will handle each MQTT event differently depending on event_id
    switch ((esp_mqtt_event_id_t)event_id) 
    {
    case MQTT_EVENT_CONNECTED:

    
        //ESP32 successfully connected to the MQTT broker
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        
        //allows the telemetry task to begin publishing data.
        mqtt_connected = true;
        break;

    case MQTT_EVENT_DISCONNECTED:

        //connection to the MQTT broker was lost >:(
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");

        //prevent telemetry from attempting to publish until the MQTT client reconnects
        mqtt_connected = false;
        break;

    case MQTT_EVENT_PUBLISHED:
        //broker acknowledged that a Quality of Service (QoS) message was published
        //msg_id identifies which MQTT message was acknowledged
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);

        //gpio_set_level(DATA_LED_GPIO, 1);
        //vTaskDelay(pdMS_TO_TICKS(100));
        //gpio_set_level(DATA_LED_GPIO, 0);

        break;

    case MQTT_EVENT_DATA:
                    
        //this runs if the ESP32 receives an MQTT message from a topic it is subscribed to then prints the received topic and payload
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        break;

    case MQTT_EVENT_ERROR:
        //a MQTT connection or transport error occurred
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");

        
        //check whether the problem occurred in the underlying TCP/TLS network connection
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) 
        {
            //ESP-IDF TLS error code
            ESP_LOGI(TAG, "Last error code reported from esp-tls: 0x%x", event->error_handle->esp_tls_last_esp_err);
            //lower level TLS library error code
            ESP_LOGI(TAG, "Last tls stack error number: 0x%x", event->error_handle->esp_tls_stack_err);
            //standard socket/network error information
            ESP_LOGI(TAG, "Last captured errno: %d (%s)", event->error_handle->esp_transport_sock_errno, strerror(event->error_handle->esp_transport_sock_errno));
        } 
            
            //the broker explicitly rejected the MQTT connection
            else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) 
            {
                ESP_LOGI(TAG, "Connection refused error: 0x%x", event->error_handle->connect_return_code);
            } 

                //catch any MQTT error type we were not specifically expecting
                else 
                {
                    ESP_LOGW(TAG, "Unknown MQTT error type: 0x%x",event->error_handle->error_type);
                }

        break;

    default:
        //log MQTT events that we are not explicitly handling
        ESP_LOGI(TAG, "Other MQTT event id: %d", event->event_id);
        break;
    }
}

//---------------------------------------------------------
//          MQTT initialization (came with template)
//--------------------------------------------------------- 
// creates the MQTT client and connects it to the broker
//
// MQTT acts as the communication layer between:
// ESP32 -> MQTT broker -> MQTTX / cloud application
static void mqtt_app_start(void)
{
    const esp_mqtt_client_config_t mqtt_cfg = {
    .broker = { 
        //broker address is configured through menuconfig ">ESP-IDF: SDK Configuration Editor (Menuconfig)"
        .address.uri = CONFIG_EXAMPLE_MQTT_BROKER_URI,
#if CONFIG_EXAMPLE_BROKER_CERTIFICATE_OVERRIDDEN
            .verification.certificate = cert_override_pem,

#elif CONFIG_EXAMPLE_CERT_VALIDATE_MOSQUITTO_CA
            .verification.certificate = (const char *)mosquitto_org_crt_start,
#else
            //use ESP-IDF's built in trusted CA bundle to validate the broker's transport layer security (TLS) certificate.
            .verification.crt_bundle_attach = esp_crt_bundle_attach,
#endif
        },
            //send MQTT keepalive ping every 30 seconds to maintain the connection
            .session = {.keepalive = 30,}, 
            //if the connections drops retry after 5 seconds
            .network = {.reconnect_timeout_ms = 5000,},
    };

    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes",esp_get_free_heap_size());

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);

    //register our event handler function so esp-idf can tell when MQTT connects, disconnects, publishes, or errors.
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler,NULL);

    esp_mqtt_client_start(mqtt_client);

    //start the seperate task that publishes telemetry every 5 seconds
    xTaskCreate(telemetry_task, "telemetry_task", 4096, NULL, 5, NULL);
}

//---------------------------------------------------------
//              Status LED initialization
//---------------------------------------------------------
static void status_led_init(void)
{
    gpio_config_t led_config =
    {
        .pin_bit_mask = (1ULL << DATA_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    ESP_ERROR_CHECK(gpio_config(&led_config));

    //temp test: force yellow LED ON continuously
    //gpio_set_level(DATA_LED_GPIO, 1);

    //start with data LED off
    gpio_set_level(DATA_LED_GPIO, 0);

    ESP_LOGI(TAG, "Data LED initialized on GPIO%d", DATA_LED_GPIO);
}

//---------------------------------------------------------
//              Thermistor monitoring task
//---------------------------------------------------------
static void thermistor_task(void *pvParameters)
{
    while (1)
    {
        int adc_raw = 0;

        esp_err_t result = adc_oneshot_read(
            adc1_handle,
            THERMISTOR_ADC_CHANNEL,
            &adc_raw
        );

        if (result == ESP_OK)
        {
            bool thermistor_valid = (adc_raw > THERMISTOR_MIN_RAW) && (adc_raw < THERMISTOR_MAX_RAW);

            if (thermistor_valid)
            {
                //thermistor circuit appears connected
                gpio_set_level(THERMISTOR_LED_GPIO, 1);

                ESP_LOGI(TAG, "Thermistor OK: ADC raw = %d", adc_raw);
            }
            else
            {
                //reading is near one of the voltage rails,
                //indicating possible open circuit,
                //disconnected sensor, or short circuit
                gpio_set_level(THERMISTOR_LED_GPIO, 0);

                ESP_LOGW(TAG, "Thermistor fault/disconnected: ADC raw = %d",adc_raw);
            }
        }
        else
        {
            //ADC itself failed to return a reading
            gpio_set_level(THERMISTOR_LED_GPIO, 0);

            ESP_LOGE(TAG,"Failed to read thermistor ADC");
        }

        //check once per second
        vTaskDelay(pdMS_TO_TICKS(THERMISTOR_CHECK_PERIOD_MS));
    }
}

static void data_led_test_task(void *pvParameters)
{
    while (1)
    {
        ESP_LOGI(TAG, "Yellow LED ON");
        gpio_set_level(DATA_LED_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(2000));

        ESP_LOGI(TAG, "Yellow LED OFF");
        gpio_set_level(DATA_LED_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}


//---------------------------------------------------------
//                          Main
//---------------------------------------------------------
// Program startup sequence:
// 1. create synchronization tools
// 2. start pulse processing task
// 3. configure GPIO4 for flowmeter pulses
// 4. initialize WiFi/network stack
// 5. connect to WiFi
// 6. start MQTT client and telemetry task
void app_main(void)
{
    ESP_LOGI(TAG, "[APP] Startup..");

    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());

    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    //mutex protects shared flow-meter data from being accessed by two tasks at exactly the same time.
    meter_mutex = xSemaphoreCreateMutex();

    if (meter_mutex == NULL) 
    {
        ESP_LOGE(TAG, "Failed to create meter mutex");
        return;
    }

    //strta the pulse processing task before enabling GPIO interupts so that the task is ready to handle pulses immediately
    xTaskCreate(pulse_task, "pulse_task", 3072, NULL, 10, &pulse_task_handle);

    //configure GPIO15 for data/activity LED
    status_led_init();


    //configure the GPIO4 for dry contact pulse detection
    flow_meter_init();

    //configure thermistor ADC and white status LED
    thermistor_init();

    //start continuous thermistor health monitoring
    xTaskCreate(
        thermistor_task,
        "thermistor_task",
        3072,
        NULL,
        5,
        NULL
    );

    //initialize esp-idf networking services
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    //connect using the wifi SSID/Password through menuconfig ">ESP-IDF: SDK Configuration Editor (Menuconfig)"
    ESP_ERROR_CHECK(example_connect());

    //start secure MQTT communication
    mqtt_app_start();


//while (1) 
//{
//    ESP_LOGI(TAG, "GPIO4 level = %d", gpio_get_level(FLOW_GPIO));
//    vTaskDelay(pdMS_TO_TICKS(1000));
//}
}