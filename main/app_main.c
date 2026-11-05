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
#include <stdatomic.h>
#include "flow_meter.h"
#include "telemetry.h"
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
#include "freertos/queue.h"

//---------------------------------------------------------
//                    Configuration
//---------------------------------------------------------
//ChillSense device identification
#define BUILDING_ID CONFIG_CHILLSENSE_BUILDING_ID
#define UNIT_ID CONFIG_CHILLSENSE_UNIT_ID
#define DEVICE_ID CONFIG_CHILLSENSE_DEVICE_ID

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

// Provisional calibration: confirm gallons per pulse with the actual meter before use.
#define GALLONS_PER_PULSE 1.0f

#define TELEMETRY_PERIOD_MS 5000

static const char *TAG = "chillsense";


//---------------------------------------------------------
//                      MQTT state
//--------------------------------------------------------- 
static esp_mqtt_client_handle_t mqtt_client = NULL;
static atomic_bool mqtt_connected = false;


//---------------------------------------------------------
//                  Flowmeter State
//--------------------------------------------------------- 
static QueueHandle_t pulse_queue = NULL;
static SemaphoreHandle_t meter_mutex = NULL;
static flow_meter_t meter = {0};
static atomic_uint pulse_queue_drops = 0;

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
//It queues the edge timestamp for the pulse processing task below.
static void flow_gpio_isr_handler(void *arg)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    // Preserve edge time and every queued edge; task scheduling must not alter GPM.
    int64_t pulse_us = esp_timer_get_time();
    if (xQueueSendFromISR(pulse_queue, &pulse_us, &higher_priority_task_woken) != pdTRUE)
    {
        atomic_fetch_add(&pulse_queue_drops, 1);
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
//This task sleeps until the GPIO interrupt queues a pulse timestamp.
//Once awake:
//1. rejects switch bounce
//2. counts the pulse
//3. calculates total gallons
//4. calculates GPM from the time between pulses
static void pulse_task(void *pvParameters)
{
    while (1)
    {
        int64_t pulse_us;
        xQueueReceive(pulse_queue, &pulse_us, portMAX_DELAY);

        xSemaphoreTake(meter_mutex, portMAX_DELAY);
        bool accepted = flow_accept_pulse(&meter, pulse_us, GALLONS_PER_PULSE);
        flow_meter_t snapshot = meter;
        xSemaphoreGive(meter_mutex);

        if (accepted)
        {
            ESP_LOGI(TAG, "Flow pulse detected: pulses=%" PRIu32
                     ", total=%.2f gal, flow=%.2f GPM",
                     snapshot.total_pulses, snapshot.total_pulses * GALLONS_PER_PULSE,
                     snapshot.flow_gpm);
        }
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
        if (atomic_load(&mqtt_connected) && mqtt_client != NULL)
        {
            xSemaphoreTake(meter_mutex, portMAX_DELAY);
            flow_meter_t snapshot = meter;
            int64_t now_us = esp_timer_get_time();
            xSemaphoreGive(meter_mutex);

            char payload[512];
            int payload_len = telemetry_payload(payload, sizeof(payload), DEVICE_ID,
                                                BUILDING_ID, UNIT_ID, &snapshot,
                                                GALLONS_PER_PULSE, now_us,
                                                atomic_load(&pulse_queue_drops));
            char mqtt_topic[128];
            int topic_len = telemetry_topic(mqtt_topic, sizeof(mqtt_topic),
                                            BUILDING_ID, UNIT_ID);
            if (payload_len < 0 || (size_t)payload_len >= sizeof(payload) ||
                topic_len < 0 || (size_t)topic_len >= sizeof(mqtt_topic))
            {
                ESP_LOGE(TAG, "Telemetry exceeds buffer; publish skipped");
                vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(TELEMETRY_PERIOD_MS));
                continue;
            }

            //publish the JSON message to the MQTT broker
            //QoS 1 means deliver this message at least once
            int msg_id = esp_mqtt_client_publish(
                mqtt_client,
                mqtt_topic,
                payload,
                0,
                1,
                0
            );

            
            if (msg_id >= 0)
            {
                // Flash for local queue acceptance; broker delivery is logged on PUBACK.
                gpio_set_level(DATA_LED_GPIO, 1);
                vTaskDelay(pdMS_TO_TICKS(100));
                gpio_set_level(DATA_LED_GPIO, 0);
            }


            if (msg_id >= 0)
            {
                ESP_LOGI(TAG, "Telemetry queued: topic=%s payload=%s | msg_id=%d",
                         mqtt_topic, payload, msg_id);
            }
            else
            {
                ESP_LOGW(TAG, "Telemetry enqueue failed: msg_id=%d", msg_id);
            }
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
        atomic_store(&mqtt_connected, true);
        break;

    case MQTT_EVENT_DISCONNECTED:

        //connection to the MQTT broker was lost >:(
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");

        //prevent telemetry from attempting to publish until the MQTT client reconnects
        atomic_store(&mqtt_connected, false);
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
            .address.uri = CONFIG_EXAMPLE_MQTT_BROKER_URI,
#if CONFIG_EXAMPLE_BROKER_CERTIFICATE_OVERRIDDEN
            .verification.certificate = cert_override_pem,
#elif CONFIG_EXAMPLE_CERT_VALIDATE_MOSQUITTO_CA
            .verification.certificate = (const char *)mosquitto_org_crt_start,
#else
            .verification.crt_bundle_attach = esp_crt_bundle_attach,
#endif
        },
        .credentials = {
            .username = CONFIG_CHILLSENSE_MQTT_USERNAME[0]
                            ? CONFIG_CHILLSENSE_MQTT_USERNAME : NULL,
            .authentication.password = CONFIG_CHILLSENSE_MQTT_PASSWORD[0]
                            ? CONFIG_CHILLSENSE_MQTT_PASSWORD : NULL,
        },
        .session.keepalive = 30,
        .network.reconnect_timeout_ms = 5000,
    };

    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes",esp_get_free_heap_size());

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);

    ESP_ERROR_CHECK(mqtt_client ? ESP_OK : ESP_ERR_NO_MEM);

    //register our event handler function so esp-idf can tell when MQTT connects, disconnects, publishes, or errors.
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));

    ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt_client));

    //start the seperate task that publishes telemetry every 5 seconds
    ESP_ERROR_CHECK(xTaskCreate(telemetry_task, "telemetry_task", 4096, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
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

    if (!valid_identity(BUILDING_ID) || !valid_identity(UNIT_ID) ||
        !valid_identity(DEVICE_ID))
    {
        ESP_LOGE(TAG, "Invalid identity: use 1-32 letters, digits, underscores or hyphens");
        return;
    }

    //mutex protects shared flow-meter data from being accessed by two tasks at exactly the same time.
    meter_mutex = xSemaphoreCreateMutex();

    if (meter_mutex == NULL) 
    {
        ESP_LOGE(TAG, "Failed to create meter mutex");
        return;
    }

    pulse_queue = xQueueCreate(32, sizeof(int64_t));
    ESP_ERROR_CHECK(pulse_queue ? ESP_OK : ESP_ERR_NO_MEM);
    // Start the consumer before enabling GPIO interrupts.
    ESP_ERROR_CHECK(xTaskCreate(pulse_task, "pulse_task", 3072, NULL, 10, NULL) == pdPASS
                        ? ESP_OK : ESP_ERR_NO_MEM);

    //configure GPIO15 for data/activity LED
    status_led_init();


    //configure the GPIO4 for dry contact pulse detection
    flow_meter_init();

    //configure thermistor ADC and white status LED
    thermistor_init();

    //start continuous thermistor health monitoring
    ESP_ERROR_CHECK(xTaskCreate(
        thermistor_task,
        "thermistor_task",
        3072,
        NULL,
        5,
        NULL
    ) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

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
