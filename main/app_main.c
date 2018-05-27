
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "nvs_flash.h"


#include "http.h"


#include "driver/i2s.h"

#include "ui.h"
#include "spiram_fifo.h"
#include "audio_renderer.h"
#include "audio_recorder.h"
#include "web_radio.h"
#include "playerconfig.h"
#include "wifi.h"
#include "app_main.h"
#include "alexa_public.h"
//#include "mdns_task.h"
#ifdef CONFIG_BT_SPEAKER_MODE
#include "bt_speaker.h"
#endif


#define WIFI_LIST_NUM   10

#define default_token \
"Atzr|IwEBIEw9D6lPzMBePedLs4njNV09LB9wqm0novQ8-UP4BP-aZKXnKrr4glGd4GoJZNHwjS0Ykc3Nk86Cr1a49BU35wqIaBDFHVrgoWb1g4FWMqBqlwyNeJO8xGhVpFlavEchcEs7w6YOsfUynKX4g2u3BJdjMDtSzcbR3CmTXwcwaPcBiM-U_sBCbTo3nftyf_BYcRh9ZiX5GoZjNN_KaHZ0BJr4cfsEJFFD4zGsoaOjJJCszk3AzIw9iGH9sQ7ae-0mHHM34wxdNtex_6FDzuGGyCFSXAduFlRzemVo-rzXXbSwiutV__yuoAlfO5pYsz0j-tYLyEPQvfNktBTKQHEF-Z-ylcgC4d8D-Mo_4sQNNaL2DgS_advUWWu7Q73sYJHe6tNBhdnpRsRKgVrN8FANAnTaceglvXCDTCD7E1GE3P7-TsphGRoGQQ9z6ICvSabKYqI"


#define TAG "main"

const char*get_auth_string ( const char*macid );

#define REFRESH_TOKEN CONFIG_ALEXA_AUTH_REFRESH_TOKEN

//#define REFRESH_TOKEN_URI "https://alexa.boeckling.net/auth/refresh/" REFRESH_TOKEN
#define REFRESH_TOKEN_URI "https://meeseeks.nullspacelabs.com/auth/refresh/" REFRESH_TOKEN

char refresh_token_uri[1024]= REFRESH_TOKEN_URI;


//Priorities of the reader and the decoder thread. bigger number = higher prio
#define PRIO_READER configMAX_PRIORITIES -3
#define PRIO_MQTT configMAX_PRIORITIES - 3
#define PRIO_CONNECT configMAX_PRIORITIES -1

// we call this 12:56AM on a friday coding 
static void alexa_task(void *pvParameters)
{
    alexa_init();
    ESP_LOGI(TAG, "alexa_task stack: %d\n", uxTaskGetStackHighWaterMark(NULL));

    // controls_init();

    vTaskDelete(NULL);
}


static void init_hardware()
{
    nvs_flash_init();

    // init UI
    // ui_init(GPIO_NUM_32);

    //Initialize the SPI RAM chip communications and see if it actually retains some bytes. If it
    //doesn't, warn user.
    if (!spiRamFifoInit()) {
        printf("\n\nSPI RAM chip fail!\n");
        while(1);
    }

    ESP_LOGI(TAG, "hardware initialized");
}

static void start_wifi()
{
    ESP_LOGI(TAG, "starting network");

    /* FreeRTOS event group to signal when we are connected & ready to make a request */
    EventGroupHandle_t wifi_event_group = xEventGroupCreate();


    /* init wifi */
    initialise_wifi(wifi_event_group);

    /* start mDNS */
    // xTaskCreatePinnedToCore(&mdns_task, "mdns_task", 2048, wifi_event_group, 5, NULL, 0);

    /* Wait for the callback to set the CONNECTED_BIT in the event group. */
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
                        false, true, portMAX_DELAY);
}

static renderer_config_t *create_renderer_config()
{
    renderer_config_t *renderer_config = calloc(1, sizeof(renderer_config_t));

    renderer_config->bit_depth = I2S_BITS_PER_SAMPLE_16BIT;
    renderer_config->i2s_num = I2S_NUM_1; // default _0
    renderer_config->sample_rate = 44100;
    renderer_config->sample_rate_modifier = 1.0;
    renderer_config->output_mode = AUDIO_OUTPUT_MODE;

    if(renderer_config->output_mode == I2S_MERUS) {
        renderer_config->bit_depth = I2S_BITS_PER_SAMPLE_32BIT;
    }

    if(renderer_config->output_mode == DAC_BUILT_IN) {
        renderer_config->bit_depth = I2S_BITS_PER_SAMPLE_16BIT;
    }

    return renderer_config;
}

static void start_web_radio()
{
    // init web radio
    web_radio_t *radio_config = calloc(1, sizeof(web_radio_t));
    radio_config->url = PLAY_URL;

    // init player config
    radio_config->player_config = calloc(1, sizeof(player_t));
    radio_config->player_config->command = CMD_NONE;
    radio_config->player_config->decoder_status = UNINITIALIZED;
    radio_config->player_config->decoder_command = CMD_NONE;
    radio_config->player_config->buffer_pref = BUF_PREF_SAFE;
    radio_config->player_config->media_stream = calloc(1, sizeof(media_stream_t));

    // init renderer
    renderer_init(create_renderer_config());

    // start radio
    web_radio_init(radio_config);
    web_radio_start(radio_config);
}




#include "common_buffer.h"
#include "url_parser.h"
#include "nghttp2/nghttp2.h"
#include "nghttp2_client.h"
#include "asio.h"
#include "asio_http.h"
#include "asio_http2.h"

static void signal_strength()
{
    start_wifi();

    wifi_ap_record_t ap_info;

    while(1) {
        esp_wifi_sta_get_ap_info(&ap_info);
        printf("rssi: %" PRIi8 "\n", ap_info.rssi);
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
}
char mac_address[ ] = "30:AE:A4:7E:5D:3C\0\0\0\0\0";

/**
 * entry point
 */
void app_main()
{
	char *p;

    ESP_LOGI(TAG, "starting app_main()");
    ESP_LOGW(TAG, "%d: - RAM left %d", __LINE__, esp_get_free_heap_size());

    //signal_strength();

    /* print MAC */
    uint8_t sta_mac[6];
    esp_efuse_mac_get_default(sta_mac);

    sprintf(&mac_address[0],  "%02X:%02X:%02X:%02X:%02X:%02X", MAC2STR(sta_mac));

    ESP_LOGE(TAG, "MAC address: %s", mac_address);

	p = get_auth_string(mac_address);

    ESP_LOGE(TAG, "Alexa Auth ID: %s  ", p) ;
    sprintf(&refresh_token_uri[0],  "https://meeseeks.nullspacelabs.com/auth/refresh/%s",  p );
    ESP_LOGE(TAG, "Alexa Refresh  %s  ", refresh_token_uri);

    init_hardware();

#ifdef CONFIG_BT_SPEAKER_MODE
    bt_speaker_start(create_renderer_config());
#else

    /*
    ESP_LOGW(TAG, "%d: - RAM left %d", __LINE__, esp_get_free_heap_size());
    //start_web_radio();
    // can't mix cores when allocating interrupts
    ESP_LOGW(TAG, "%d: - RAM left %d", __LINE__, esp_get_free_heap_size());
    */
    renderer_init(create_renderer_config());
    audio_recorder_init();
    xTaskCreatePinnedToCore(&alexa_task, "alexa_task", 16384, NULL, 1, NULL, 1);
#endif

    ESP_LOGW(TAG, "%d: - RAM left %d", __LINE__, esp_get_free_heap_size());
    // ESP_LOGI(TAG, "app_main stack: %d\n", uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
}

// the it's less that 24 hours to go and hacks

const char authid[300][700] = {

#include "ids.h"

};

#define ARRAY_LENGTH(array) (sizeof((array))/sizeof((array)[0]))

const char macids[][18] = {
	#include "mac.h"
};

const char*get_auth_string ( const char*macid )
{

    for ( unsigned short i = 0; i < ARRAY_LENGTH ( macids ); i++ ) {

        // if matches, return an id
        if ( 0 == strcmp ( macids[i], macid ) ) {
                ESP_LOGI(TAG, "auth id index : %d\n",   i);
            return &authid[i % ARRAY_LENGTH ( authid )] ;
        };
    }
        ESP_LOGI(TAG, "auth id index : default%d \n", 0 );


return default_token;

srand( clock() ) ;

    // default if mac not found, unreliable
    switch ( rand() % 6 ) {
        case 0:
            return "Atzr|IwEBIAAFqAey8ptXKdeW22pDqsxHfks5zyCVeRsZtBcjLzd-DA999hA4Coykyf_2WB1INjwko7OJPx7_l6Ypw7ZtUn0NsMLvp89kn3dTJWhQRvro_YZJstaZ3ukHZtOQbKVP5OnOZZEMq06wNqmxdr8aeqW3_iTiBaxrmVm63mZZxep6-Szom3BMSNCALiIv7j0kwRovIYF-diLYvhL4V0_LCZPzZRdGKWlGR411y8kEOxnfvRRTp5L90Yoi7qsWJek7lB6cK7hN-htHyMmkPYPPILAtgIS0I0o8za5d_4YeG6B4in9u2aNMhJVfr7AjnN7EDZglKK9C-vNdBx-rwkUkPgRlM1ZmWYid73hry5EeDNPZX_VvKH1zJ7ownSD5HJKKKyNNEUwzt-8A2yxiJIytgea9V3M3d3ORfqHW5myBqTrm_-qK9juv5OTbQTEaOBU74NA";

        case 1:
            return "Atzr|IwEBIHlxv5enNJ1nG6mLNERH-bMwAdWeK8C_oXORQssGr3GTM_-86nnQwNMlLcMgb2_OmsVMqZDCRQNkY4SJABrklP0jUgIpY_41w1Orox0tOrZF3-gkLpDgSFsfYoA8l5SNrbNLnkuFlBrCyJAETo3tzKpEWh6fqKfFhq3_BBz6adKIq1Uh0492kFGFhQ5ShyunVASEQRmW2OBWfeB_djQygvv0ZJvbFQGLhBtIFRipiXN-9ohk2VqQAUXkByCgO5JFjOYQixunCRkprx-Y7E2Sfmt91jMfDoxgekefIBe5slqIv-N9wg2cIg97fKEQmxOzMm13DaKQINe_vFAz0CIkhrqpr8L4QgxYLDXtPARYSpAuqcWsxhcBt0QiyGpQDiWT64MxJ_LVSw8IL5P75_A0BMKbTV4eGnIc0U0NWCU2Yd2c4UU3yg-47hQLoSYuD31Foy0";

        case 2:
            return "Atzr|IwEBICLSGCTMDlbH6jpv7xs9yvpcD1qmYRBYCh_90v7600ysITqrHz27UZC05_ydHgWEh8el6teTpVF8mYPQ30Flzs4nUF8EJPQhjNjgrX0ng2NK2CKI-T0yc2xZHXHYF8mgV_FKGgyxdSe1uHHhP-khdM_Uu5NQZy5Yxv1edzmpEXOoroLEX8hZTgoMuKhNw-YXdSu7-eFxozI5ZhaEgYx3DgHuYdBC8p2ICaWqZz-mtTaA6Uo2WtD9xVM3YcdQa4hB8oAhVfQT1-jhcRsrHmWx1snuWi5Eop2nEReKET5VkdRoPWfH252OlzFz4JWdJXiUHcBP9_mdouhYYzXKmjvIPiCI57Mh-Jkbnor0OmXxp8cAk7I5T7e3D9woeae6bmh4yofCTw3Sc-2HaKciypGin_eRiDiOsXLc3m6cOmke4g2v1xKxorPtKM4JprVYsGQq1Nw";

        case 3:
            return "Atzr|IwEBIL8PwumKxtLi9yVIMjJSjYBHC9KFnUO6cY5NMrddxq0OndatIKfAJWjQx1VEyqSeMmrodht0_BS0JKKxmKwcZk6lCmHSpV0AQrx9fPQTWiyn7YT8cKsbMeBNRNLrbU5_0bVfvLJivxXoZcdSSS3umhdQMs2J521gzs_JbljmLO8QKAToSvK5eth_IppA195S45PUVi4w2lSK4hF3aGalB2WignazsXvExt32ojFD_1_OxZFaCKDLdds7yJSXxXnSK-YQ862T4tG4Bwjabdlt0Bw2ID03mCaG7c3r2tAinAJd2RiX4KB3qssyE92aBHsmPQKD7bPV_qmiosIaQKbS_cfYYGg98blJ1hORHxk3tvxmsN3GFQA3lmxS1KuC3YBkeMwRMgAW4oRw-lD8JWvTt2wUBOnPhRuYd1-tMcPQGz4t_6g1pehwqCwXPXQEnjGZxbo";

        case 4:
            return "Atzr|IwEBIMhoySGhvPkw1eOlLtH_N76_Dtv9xwdSVBLQQ1wRVkNzCyyFLJlb-v3bdM0lALvYCUczkQ8XaohRBEaSyisism94c8keXbHwaWbdz2SldvPT_WFENq_ZzHWrBtPtTa3U-6ht9vPRQkOLtxwgsRTKHEfB4J8ymWJ1aVPoV9Bsnj56umGvHG-zY03h3-HNuQxGxWTfSqKnoKCf6hcVKSZ2PMGt-N9coPTzk26Uur2U1_7aaoV7aYeVq2VXGBjDV_1qkxBYGTqXzzIAA9NWM7L_mpRqYVMtLAqe69jtBfBi-ZFaqRU4EUsVR0ZGPbwrf5EKJnc02TmjfV-Zld0s8IgVPP51lI_ghw1PDk4JqCaB6pImMUiabNnQ2wAmjcSm-J-pKZ293nZnuAcUcNDHXF1B2qPA-vaZ_qIXU-iOS_zMfXAXdvE4P6ncAM5ykFaePdCNS8Y";

        default:
            return "Atzr|IwEBIMOMj3mViKbmZGYhiHYKZoCRUhN7FEh3algDbXY71epSr_ormtod_F2jMbnCQ8uDM2-Q4_76q_B7valbGDRaCYalZFjc8rXH5ygRKnR0DhzFknsK2HR9Vl7XLJEf5Gk43xBp6jJ0585O5bjreEY3dTtPHT5ZsngUzSrxogkMWjc3hj1cy61e1nnrH0xngX61Mcx7EWBelR1DqpWKNjkvF4x635IOoFfT7ipEsQFxgWRoy5tYM9UR_KqrLeTwfw7Agzdz8PboS0WhqP0b6u9chdE2IZc31RKdlPdx-gaf7xQdXltX1zVFsFcRtUagSyhD3KprgzvtzIwearO_2OJrR6yMtfnxaJFnhDahOcQbYTSRIVOOAWbVxar34GSVge9sldIsI6WjJgwBnJRBlYsRm8-l8IfQHM_mopkNB03OjVNUox97xWVTIvJO1-mIjUfctSg";
    }

}

