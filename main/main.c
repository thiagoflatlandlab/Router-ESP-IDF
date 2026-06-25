#include <stdio.h>
#include <string.h>
#include "esp_eth_mac_esp.h"
#include "esp_log_buffer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_eth.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lora.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "mbedtls/gcm.h"
#include "esp_system.h"
#include "esp_bt.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

// --- DEFINES  ---
#define AWS_IOT_SUBSCRIBE_TOPIC "esp32/sub"
#define AWS_IOT_STARTUP_TOPIC   "esp32/startup"
#define THINGNAME               "SeuThingName"
#define MAX_PAYLOAD_SIZE 1024

// LoRa Pins 
#define LORA_SCK  14
#define LORA_MISO 35
#define LORA_MOSI 13
#define LORA_SS   5
#define LORA_RST  4
#define LORA_DIO0 34

// Ethernet LAN8720 Pins
#define ETH_MDC_PIN     23
#define ETH_MDIO_PIN    18
#define ETH_PHY_ADDR    1

// AES-GCM
#define IV_LENGTH 12
#define TAG_LENGTH 8

static const char *TAG = "NTRIP_ROUTER";

// Global Variables
static char ggaSentence[128] = {0};
static char casterHost[64] = {0};
static uint16_t casterPort = 2102;
static char casterUser[32] = {0};
static char casterUserPW[32] = {0};
static char mountPoint[32] = {0};
static bool got_startup_data = false;

// Flags de rede
volatile bool ethConnected = false;
volatile bool wifiConnected = false;

// Ponteiros atualizados
extern const uint8_t aws_root_ca_pem_start[] asm("_binary_AmazonRootCA1_pem_start");
extern const uint8_t certificate_pem_crt_start[] asm("_binary_DeviceCertificate_crt_start");
extern const uint8_t private_pem_key_start[] asm("_binary_PrivateKey_key_start");
// --- DECLARAÇÃO DE FUNÇÕES ---
void sendLoRa(uint8_t *data, int len);
void taskNTRIP(void *pvParameters);

// --- HANDLERS DE REDE ---
static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == ETH_EVENT) {
        switch (event_id) {
            case ETHERNET_EVENT_CONNECTED:
                ESP_LOGI(TAG, "[ETH] Cabo conectado fisicamente.");
                break;
            case ETHERNET_EVENT_DISCONNECTED:
                ESP_LOGW(TAG, "[ETH] ALERTA: Cabo desconectado!");
                ethConnected = false;
                if (!wifiConnected) {
                    ESP_LOGI(TAG, "[REDE] Acionando Wi-Fi como plano de contingência...");
                    esp_wifi_start();
                    esp_wifi_connect();
                }
                break;
        }
    } else if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            ESP_LOGI(TAG, "[WIFI] Interface Iniciada.");
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            ESP_LOGI(TAG, "[WIFI] Desconectado!");
            wifiConnected = false;
            if (!ethConnected) {
                esp_wifi_connect();
            }
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_ETH_GOT_IP) {
            ESP_LOGI(TAG, "[ETH] IP recebido com sucesso");
            ethConnected = true;
            if (wifiConnected) {
                ESP_LOGI(TAG, "[REDE] Ethernet priorizado! Desativando rádio Wi-Fi...");
                esp_wifi_stop(); // Desativa Wi-Fi se ETH funcionou
                wifiConnected = false;
            }
        } else if (event_id == IP_EVENT_STA_GOT_IP) {
            ESP_LOGI(TAG, "[WIFI] IP recebido com sucesso");
            wifiConnected = true;
        }
    }
}

// --- HANDLER MQTT (AWS IoT) ---
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "AWS IoT Conectado!");
            esp_mqtt_client_subscribe(client, AWS_IOT_SUBSCRIBE_TOPIC, 1);
            esp_mqtt_client_subscribe(client, AWS_IOT_STARTUP_TOPIC, 1);
            break;
            
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "Nova mensagem: %.*s", event->topic_len, event->topic);
            
            // Trava de segurança para evitar Buffer Overflow na stack
            if (event->data_len >= MAX_PAYLOAD_SIZE) {
                ESP_LOGE(TAG, "Falha: Payload excedeu o tamanho máximo (%d bytes)", MAX_PAYLOAD_SIZE);
                break;
            }

            char payload[MAX_PAYLOAD_SIZE];
            memcpy(payload, event->data, event->data_len);
            payload[event->data_len] = '\0';

            if (strncmp(event->topic, AWS_IOT_STARTUP_TOPIC, event->topic_len) == 0) {
                cJSON *doc = cJSON_Parse(payload);
                if (doc != NULL) {
                    cJSON *host = cJSON_GetObjectItem(doc, "casterHost");
                    if (cJSON_IsString(host)) strlcpy(casterHost, host->valuestring, sizeof(casterHost));
                    
                    cJSON *user = cJSON_GetObjectItem(doc, "casterUser"); 
                    if (cJSON_IsString(user)) strlcpy(casterUser, user->valuestring, sizeof(casterUser));
                    
                    cJSON *pw = cJSON_GetObjectItem(doc, "casterUserPW");
                    if (cJSON_IsString(pw)) strlcpy(casterUserPW, pw->valuestring, sizeof(casterUserPW));
                    
                    cJSON *mp = cJSON_GetObjectItem(doc, "mountPoint");
                    if (cJSON_IsString(mp)) strlcpy(mountPoint, mp->valuestring, sizeof(mountPoint));
                    
                    cJSON *gga = cJSON_GetObjectItem(doc, "ggaSentence"); 
                    if (cJSON_IsString(gga)) strlcpy(ggaSentence, gga->valuestring, sizeof(ggaSentence));
                    
                    cJSON *port = cJSON_GetObjectItem(doc, "casterPort");
                    if (cJSON_IsNumber(port)) casterPort = port->valueint;

                    ESP_LOGI(TAG, "Dados de Startup Recebidos: %s:%d", casterHost, casterPort);
                    got_startup_data = true;
                    esp_mqtt_client_unsubscribe(client, AWS_IOT_STARTUP_TOPIC);
                    
                    cJSON_Delete(doc); // Apenas o parser do cJSON ainda lida com alocação interna
                } else {
                    ESP_LOGE(TAG, "Falha ao fazer o parse do JSON");
                }
            }
            break;
        default:
            break;
    }
}

// --- TASK NTRIP COM SUPORTE A HTTPS (TLS) ---
void taskNTRIP(void *pvParameters) {
    for (;;) {
        // Aguarda os dados vindos do MQTT antes de iniciar
        if (!got_startup_data || strlen(ggaSentence) < 15) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

// Configuração do TLS alocada estaticamente na Stack
        esp_tls_cfg_t cfg = {
            .crt_bundle_attach = esp_crt_bundle_attach, // Valida o certificado HTTPS automaticamente
            .timeout_ms = 8000,                         // Timeout para operações de rede
        };

        ESP_LOGI(TAG, "Tentando conectar ao Caster NTRIP seguro: %s:%d...", casterHost, casterPort);

        // 1. Inicializa o handle (Estrutura opaca do ESP-IDF)
        esp_tls_t *tls = esp_tls_init();
        if (tls == NULL) {
            ESP_LOGE(TAG, "Falha ao inicializar o contexto TLS");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        // 2. Abre a conexão TLS síncrona (Atenção ao 5º argumento e ao retorno int)
        int tls_status = esp_tls_conn_new_sync(casterHost, strlen(casterHost), casterPort, &cfg, tls);

        if (tls_status == 1) {
            ESP_LOGI(TAG, "Conectado ao Caster NTRIP via HTTPS (TLS Estabelecido)!");

            // Criptografia Basic Auth: Encode Base64 (Alocado estaticamente na Stack)
            char userCredentials[128];
            snprintf(userCredentials, sizeof(userCredentials), "%s:%s", casterUser, casterUserPW);
            
            unsigned char auth_b64[256];
            size_t auth_len = 0;
            mbedtls_base64_encode(auth_b64, sizeof(auth_b64), &auth_len, (const unsigned char*)userCredentials, strlen(userCredentials));
            auth_b64[auth_len] = '\0'; // Garante a terminação correta da string

            // Envia o Request HTTP sobre a camada TLS
            char request[512];
            snprintf(request, sizeof(request), "GET /%s HTTP/1.0\r\nAuthorization: Basic %s\r\n\r\n", mountPoint, auth_b64);
            esp_tls_conn_write(tls, request, strlen(request));

            // Envia o primeiro GGA imediatamente para inicializar fluxos VRS
            char gga_req[150];
            snprintf(gga_req, sizeof(gga_req), "%s\r\n", ggaSentence);
            esp_tls_conn_write(tls, gga_req, strlen(gga_req));
            ESP_LOGI(TAG, "GGA Inicial enviado cifrado!");

            // Controle e buffers na Stack
            uint8_t loraBuffer[200];
            int bufferIdx = 0;
            bool headerPassed = false;
            const int maxTimeBeforeHangup_ms = 50000; 
            
            uint32_t lastReceivedRTCM_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            uint32_t lastSentGGA_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

            // Loop de processamento de stream criptografada
            while (1) {
                uint8_t c;
                // Leitura de 1 byte por vez através do túnel TLS
                int rx_bytes = esp_tls_conn_read(tls, &c, 1); 

                if (rx_bytes > 0) {
                    if (!headerPassed) {
                        if (c == 0xD3) { // Identificador de início do frame RTCM3
                            headerPassed = true;
                            loraBuffer[bufferIdx++] = c; 
                        }
                    } else {
                        loraBuffer[bufferIdx++] = c;
                        if (bufferIdx >= 200) { 
                            sendLoRa(loraBuffer, bufferIdx);
                            bufferIdx = 0; 
                        }
                    }
                    lastReceivedRTCM_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
                } else if (rx_bytes == 0) {
                    ESP_LOGW(TAG, "Conexão encerrada remotamente pelo servidor Caster HTTPS.");
                    break;
                } else {
                    // Tratamento de timeouts e estados do canal TLS
                    if (rx_bytes == ESP_TLS_ERR_SSL_WANT_READ || rx_bytes == ESP_TLS_ERR_SSL_WANT_WRITE) {
                        vTaskDelay(pdMS_TO_TICKS(10)); 
                    } else {
                        ESP_LOGE(TAG, "Erro crítico na camada TLS: %d", rx_bytes);
                        break; 
                    }
                }

                uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
                
                // Envio periódico do GGA
                if ((now - lastSentGGA_ms) > 10000) {
                    esp_tls_conn_write(tls, gga_req, strlen(gga_req));
                    lastSentGGA_ms = now;
                }

                // Watchdog de software interno para Caster travado
                if ((now - lastReceivedRTCM_ms) > maxTimeBeforeHangup_ms) {
                    ESP_LOGW(TAG, "Timeout: Mais de 50s sem dados RTCM válidos via HTTPS.");
                    break; 
                }
            }

            esp_tls_conn_destroy(tls);
            ESP_LOGW(TAG, "Sessão TLS encerrada. Reiniciando em breve...");
        } else {
            ESP_LOGE(TAG, "Falha ao estabelecer handshake TLS com o Host. Codigo de erro: %d", tls_status);
            esp_tls_conn_destroy(tls); 
        }

        vTaskDelay(pdMS_TO_TICKS(5000)); // Delay de recuo antes de tentar reconectar
    }
}

// Função simétrica: aplicar a mesma função no RX restaura os dados originais
void shuffle_sectors(uint8_t *data, int len) {
	
	//ESP_LOG_BUFFER_HEX(TAG, data, len);
    if (len < 4) return; // Muito pequeno para fatiar, ignora

    int sector_size = len / 4;
    uint8_t temp;
    
    // 1. Embaralha Setor 0 com Setor 2
    for(int i = 0; i < sector_size; i++) {
        temp = data[i];
        data[i] = data[i + 2 * sector_size];
        data[i + 2 * sector_size] = temp;
    }
    
    // 2. Embaralha Setor 1 com Setor 3
    for(int i = 0; i < sector_size; i++) {
        temp = data[i + sector_size];
        data[i + sector_size] = data[i + 3 * sector_size];
        data[i + 3 * sector_size] = temp;
    }

    // 3. Ofusca o "resto" (bytes finais que não couberam nos setores exatos) com um XOR simples
    int remainder_start = 4 * sector_size;
    for(int i = remainder_start; i < len; i++) {
        data[i] ^= 0xAA; // Chave XOR fixa para os bytes finais
    }
}

// Abstração do LoRa - Versão Rápida com Embaralhamento
void sendLoRa(uint8_t *rtcm_data, int rtcm_len) {
    // 1. Verificação de tamanho (Max LoRa payload é 255 bytes)
    if (rtcm_len > 255) {
        ESP_LOGE("LORA", "Tamanho do RTCM excede o buffer do LoRa (Max 255).");
        return;
    }

    // 2. Cria um buffer temporário para não alterar o buffer original
    uint8_t payload[255];
    memcpy(payload, rtcm_data, rtcm_len);

    // 3. Aplica o embaralhamento de setores
    shuffle_sectors(payload, rtcm_len);

    // 4. Envia diretamente para o hardware LoRa
    lora_send_packet(payload, rtcm_len);
    
    ESP_LOGI("LORA", "Pacote embaralhado enviado! Tamanho total: %d bytes", rtcm_len);
}

// --- CONFIGURAÇÃO DO NIMBLE (BLE UART) ---
uint16_t ble_conn_handle;

static int ble_uart_rx_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    // Essa linha vai provar se o celular está conseguindo acessar a característica!
    ESP_LOGW("BLE_DEBUG", "FUI TOCADO! Operacao tentada: %d", ctxt->op);

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t rx_len = ctxt->om->om_len;
        const uint8_t *rx_data = ctxt->om->om_data;
        ESP_LOGI("BLE_RX", "Mensagem Recebida (%d bytes): %.*s", rx_len, rx_len, rx_data);
    }
    return 0;
}

static int ble_uart_tx_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        const char *ready_msg = "OK";
        os_mbuf_append(ctxt->om, ready_msg, strlen(ready_msg));
    }
    return 0;
}

// Tabela GATT Completa (Serviço UART + RX + TX)
static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID128_DECLARE(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E), // Serviço Nordic UART
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                // Característica RX (O ESP32 RECEBE por aqui - UUID final 02)
                .uuid = BLE_UUID128_DECLARE(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E), 
                .access_cb = ble_uart_rx_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                // Característica TX (O ESP32 ENVIA por aqui - UUID final 03)
                .uuid = BLE_UUID128_DECLARE(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E), 
                .access_cb = ble_uart_tx_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {0} // Fim das características
        }
    },
    {0} // Fim dos serviços
};

static int ble_gap_event(struct ble_gap_event *event, void *arg);

// Função auxiliar para iniciar/reiniciar a visibilidade do Bluetooth
static void start_ble_advertising(void) {
    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    uint8_t own_addr_type;
    ble_hs_id_infer_auto(0, &own_addr_type);
    
    ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event, NULL);
    ESP_LOGI("BLE_GAP", "Advertising Iniciado! Visivel para pareamento.");
}

// Callback de Eventos de Conexão do BLE
static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                ESP_LOGI("BLE_GAP", "Cliente Conectado!");
                ble_conn_handle = event->connect.conn_handle;
            } else {
                // Se a conexão falhar, volta a ficar visível
                start_ble_advertising(); 
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI("BLE_GAP", "Cliente Desconectado!");
            // Reinicia a propaganda para outro celular poder achar
            start_ble_advertising();
            break;
    }
    return 0;
}

// Tarefa que mantém o Host NimBLE rodando
void ble_host_task(void *param) {
    ESP_LOGI("BLE_SYS", "NimBLE Host Task Iniciada");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// Sincronização inicial
static void ble_on_sync(void) {
    ble_svc_gap_device_name_set("ESP32_NTRIP_ROUTER");
    start_ble_advertising();
}
// --- FIM DA CONFIGURAÇÃO DO NIMBLE ---

// --- SETUP PRINCIPAL ---
void app_main(void) {
	// Inicialização LoRa
    if (lora_init() == 0) {
        ESP_LOGE("LORA", "Falha ao iniciar LoRa!");
        while (1) vTaskDelay(10);
    }
    
    lora_set_frequency(927800000); // 927.8 MHz
    lora_set_tx_power(17);         // Substitui LoRa.setTxPower(20) - 17 é o máx seguro no chip padrão
    lora_set_bandwidth(7);         // 7 = 125E3 no chip SX1276
    lora_set_coding_rate(1);       // CodingRate 4/8
    lora_set_spreading_factor(7);
    lora_set_sync_word(0xF3);
    lora_enable_crc();
    
    ESP_LOGI("LORA", "[OK] LoRa iniciado.");
    
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGI(TAG, "\n\n--- INICIANDO SISTEMA (ETH/WIFI REDUNDANTE) ---"); //

	// --- INICIALIZAÇÃO DO NIMBLE ---
    ESP_LOGI(TAG, "Iniciando NimBLE...");
    
    // Absolutely NO esp_bt_controller_mem_release here!
    // Absolutely NO esp_bt_controller_init here!
    
    // NimBLE handles the controller initialization natively using your menuconfig settings
    ESP_ERROR_CHECK(nimble_port_init());
    
    // Configura a tabela de serviços (GATT)
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(gatt_svr_svcs);
    ble_gatts_add_svcs(gatt_svr_svcs);

    // Configura a função que inicia a propaganda quando o sistema sincroniza
    ble_hs_cfg.sync_cb = ble_on_sync;

    // Inicia a tarefa do NimBLE no FreeRTOS
    nimble_port_freertos_init(ble_host_task);
    // 1. Inicialização Ethernet LAN8720
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&cfg);
    esp_netif_set_hostname(eth_netif, "esp32-ntrip-router");

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = ETH_PHY_ADDR;
    phy_config.reset_gpio_num = -1; // Sem pino de reset definido 

    eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp32_emac_config.smi_gpio.mdc_num = ETH_MDC_PIN;
    esp32_emac_config.smi_gpio.mdio_num = ETH_MDIO_PIN;
    esp32_emac_config.clock_config.rmii.clock_mode = EMAC_CLK_OUT;
    esp32_emac_config.clock_config.rmii.clock_gpio = EMAC_CLK_OUT_180_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_config);
    esp_eth_handle_t eth_handle = NULL;
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));
    
    // Registrando os Eventos de Rede
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));

    // Inicia Ethernet
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    // 2. Inicialização Wi-Fi de Contingência (WIFI_STA)
    esp_netif_t *wifi_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(wifi_netif, "esp32-ntrip-router");
    wifi_init_config_t cfg_wifi = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg_wifi));
    
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "Flatland-Lab(US)",         // Substitua pela variável do secrets.h 
            .password = "Flatland+2025!",    // Substitua pela variável do secrets.h 
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    
    // Aguarda Rede (Failover Lógica)
    vTaskDelay(pdMS_TO_TICKS(8000));
    if (!ethConnected) {
        ESP_LOGI(TAG, "Sem cabo Ethernet. Fazendo fallback para Wi-Fi...");
        ESP_ERROR_CHECK(esp_wifi_start());
        esp_wifi_connect();
    }

    while (!ethConnected && !wifiConnected) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "Rede estabelecida com sucesso!");

    // 3. Inicialização MQTT (AWS)
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtts://a3kzwueok42ssw-ats.iot.us-east-1.amazonaws.com:8883",
        .credentials.client_id = THINGNAME,
        .credentials.authentication.certificate = (const char *)certificate_pem_crt_start,
        .credentials.authentication.key = (const char *)private_pem_key_start,
        .broker.verification.certificate = (const char *)aws_root_ca_pem_start,
    };
    
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);

    // 4. Criação das Tasks
    xTaskCreatePinnedToCore(taskNTRIP, "TaskNTRIP", 8192, NULL, 1, NULL, 0);
}