idf_component_register(SRCS "main.c" "lora.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_netif esp_eth esp_wifi nvs_flash mqtt json mbedtls fatfs lwip bt
                    EMBED_TXTFILES "certs/AmazonRootCA1.pem"
                                   "certs/DeviceCertificate.crt"
                                   "certs/PrivateKey.key")