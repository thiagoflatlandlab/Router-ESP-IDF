#ifndef LORA_H
#define LORA_H

#include <stdint.h>

#define CONFIG_MISO_GPIO 35
#define CONFIG_MOSI_GPIO 13
#define CONFIG_SCK_GPIO  14

#define CONFIG_CS_GPIO   33
#define CONFIG_RST_GPIO  32

#ifndef CONFIG_SPI2_HOST
#define CONFIG_SPI2_HOST 1
#endif

void lora_reset(void);
void lora_explicit_header_mode(void);
void lora_implicit_header_mode(int size);
void lora_idle(void);
void lora_sleep(void);
void lora_receive(void);
void lora_set_tx_power(int level);
void lora_set_frequency(long frequency);
void lora_set_spreading_factor(int sf);
int lora_get_spreading_factor(void);
void lora_set_dio_mapping(int dio, int mode);
int lora_get_dio_mapping(int dio);
void lora_set_bandwidth(int sbw);
int lora_get_bandwidth(void);
void lora_set_coding_rate(int cr);
int lora_get_coding_rate(void);
void lora_set_preamble_length(long length);
long lora_get_preamble_length(void);
void lora_set_sync_word(int sw);
void lora_enable_crc(void);
void lora_disable_crc(void);
int lora_init(void);
void lora_send_packet(uint8_t *buf, int size);
int lora_receive_packet(uint8_t *buf, int size);
int lora_received(void);
int lora_get_irq(void);
int lora_packet_lost(void);
int lora_packet_rssi(void);
float lora_packet_snr(void);
void lora_close(void);
void lora_dump_registers(void);
void lora_enable_ldro(void);
void lora_disable_ldro(void);

#endif