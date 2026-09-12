#include <stdio.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "cJSON.h"

//WiFi Credentials:-
#define WiFi_SSID "USER_WIFI_SSID"
#define WiFi_PASS "USER_WIFI_PASSWORD"

//message 
#define MAX_MESSAGE_LENGTH 256
#define dot_interval 300
const char *TAG = "Morse Transmitter";

//LED
#define LED_PIN 37

//mqtt specific variables:-
esp_mqtt_client_handle_t mqtt_client = NULL;
bool mqtt_connected = false;

//Variables:-
char message[MAX_MESSAGE_LENGTH];
bool message_stored = false;


//strtok variables:-
const char delimiter[] = " ";
char *portion;

//Morse code arrays:-
const char *alphabets[26] = {".-", "-...", "-.-.", "-..", ".", "..-.", 
                             "--.", "....", "..", ".---", "-.-", ".-..", 
                             "--", "-.", "---", ".--.", "--.-", ".-.", 
                             "...", "-", "..-", "...-", ".--", "-..-",
                             "-.--", "--.."
                            };
const char *numbers[10] = {"-----", ".----", "..---", "...--", "....-",
                           ".....", "-....", "--...", "---..", "----."
                          };
const char *question_mark = "..--..";
const char *exclamation_mark = "-.-.--";
const char *full_stop = ".-.-.-";
const char *comma = "--..--";
const char *semi_colon = "-.-.-.";
const char *colon = "---...";
const char *slash = "-..-.";

//Prototypes:-
static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
);
void wifi_init_sta();

static void mqtt_event_handler(
    void *handler_args,
    esp_event_base_t base,
    int32_t event_id,
    void *event_data
);
void mqtt_init(void);

void process_message_tokenisation(void);
void process_message_words(char *portio);
void analyze(char *read); 

//Actual functions begin from here:-
static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START){
        ESP_LOGI(TAG, "Wi-Fi started, connecting...");
        esp_wifi_connect();
    }

    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED){
        ESP_LOGI(TAG, "Wi-Fi disconnected. Reconnecting...");
        esp_wifi_connect();
    }

    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP){
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        mqtt_init();
        ESP_LOGI(TAG, "Wi-Fi connected. IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

void wifi_init_sta()
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &wifi_event_handler,
            NULL,
            NULL
        )
    );

    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &wifi_event_handler,
            NULL,
            NULL
        )
    );

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WiFi_SSID,
            .password = WiFi_PASS,
        },
    };

    ESP_ERROR_CHECK(
        esp_wifi_set_mode(WIFI_MODE_STA)
    );

    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_STA,
            &wifi_config
        )
    );

    ESP_ERROR_CHECK(
        esp_wifi_start()
    );
}

static void mqtt_event_handler(
    void *handler_args,
    esp_event_base_t base,
    int32_t event_id,
    void *event_data
)
{

    esp_mqtt_event_handle_t event = event_data;

    switch (event_id){
        case MQTT_EVENT_CONNECTED:
        {
            ESP_LOGI("MQTT", "Connected to broker");
            mqtt_connected = true;

            int msg_id = esp_mqtt_client_subscribe(
                mqtt_client,
                "morse",
                0
            );
            ESP_LOGI("MQTT", "Subscribe requested, msg_id=%d", msg_id);
            break;
        }

        case MQTT_EVENT_SUBSCRIBED:
        {
            ESP_LOGI("MQTT", "Subscription acknowledged");
            break;
        }

        case MQTT_EVENT_DISCONNECTED:
        {    ESP_LOGI("MQTT", "Disconnected from broker");
            mqtt_connected = false;
            break;
        }

        case MQTT_EVENT_ERROR:
        {    ESP_LOGE("MQTT", "MQTT error");
            break;
        }

        
    case MQTT_EVENT_DATA:
    {
        printf("\n--- DATA EVENT ---\n");

        printf("Raw message: %.*s\n",
           event->data_len,
           event->data);

        cJSON *root = cJSON_ParseWithLength(
        event->data,
        event->data_len
        );

        if (root == NULL) {
            printf("JSON parsing FAILED\n");
            break;
        }

        printf("JSON parsing SUCCESS\n");

        cJSON *message_item = cJSON_GetObjectItem(root, "message");

        if (message_item == NULL) {
            printf("'message' item NOT FOUND\n");
        }

        else{
            printf("'message' item found\n");

            if(cJSON_IsString(message_item)) {
                printf("valuestring = %s\n", message_item->valuestring);

                strncpy(
                message,
                message_item->valuestring,
                MAX_MESSAGE_LENGTH - 1
                );

                message[MAX_MESSAGE_LENGTH - 1] = '\0';

                printf("global message = %s\n", message);

                message_stored = true;

                printf("message_stored = %d\n", message_stored);

                process_message_tokenisation();

                printf("\nreturned from print_message()\n");
            }
            else{
                printf("'message' is NOT a string\n");
            }
        }

        cJSON_Delete(root);

        break;
    }
        default:
        {    
            break;
        }
    }
}

void mqtt_init(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtt://YOUR_PC_IP"
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);

    esp_mqtt_client_register_event(
        mqtt_client,
        ESP_EVENT_ANY_ID,
        mqtt_event_handler,
        NULL
    );

    esp_mqtt_client_start(mqtt_client);
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);

    esp_err_t ret = nvs_flash_init();
     if(ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND){
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }

        ESP_ERROR_CHECK(ret);

    wifi_init_sta();

    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    
    vTaskDelay(1000 / portTICK_PERIOD_MS);
}


void process_message_tokenisation(void){
    printf("\nentered msg processing.\n");
    if(message_stored){
        portion = strtok(message, delimiter);
        printf("Portion:- %s", portion);
        while(portion != NULL){
            process_message_words(portion);
            portion = strtok(NULL, delimiter);
            printf("\nnext portion:- %s\n", portion);
            if (portion != NULL) {
                // There is another word, so insert a word gap
                vTaskDelay((dot_interval * 7) / portTICK_PERIOD_MS);
            }
        }
    }
}

void process_message_words(char *portio){
    for(int i = 0; portio[i] != '\0'; i++){
        analyze(&portio[i]);
    }
}

void analyze(char *read){
    char input = toupper(*read);
    uint8_t n;
    const char* code = NULL;
    if(input >= 'A' && input <= 'Z'){
        n = input - 'A';
        code = alphabets[n];
    }
    else if(input >= '0' && input <= '9'){
        n = input - '0';
        code = numbers[n];
    }
    else{
        switch(input){
            case '?':
            {
                code = question_mark;
                break;
            }
            case '!':
            {
                code = exclamation_mark;
                break;
            }
            case '.':
            {
                code = full_stop;
                break;
            }
            case ',':
            {
                code = comma;
                break;
            }
            case ';':
            {
                code = semi_colon;
                break;
            }
            case ':':
            {
                code = colon;
                break;
            }
            case '/':
            {
                code = slash;
                break;
            }
        }
    }
    if(code == NULL){
        return;
    }
    for(int i = 0; code[i] != '\0'; i++){
        if(code[i] == '.'){
            gpio_set_level(LED_PIN, 1);
            vTaskDelay(dot_interval / portTICK_PERIOD_MS);
        }
        if(code[i] == '-'){
            gpio_set_level(LED_PIN, 1);
            vTaskDelay((dot_interval * 3) / portTICK_PERIOD_MS);
        }

        gpio_set_level(LED_PIN, 0);

        vTaskDelay(dot_interval / portTICK_PERIOD_MS);
    }

    vTaskDelay((dot_interval * 2) / portTICK_PERIOD_MS);
}