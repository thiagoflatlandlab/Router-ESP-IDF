# ESP32 Secure NTRIP to LoRa Router

Este projeto implementa um roteador NTRIP seguro baseado em ESP32. O sistema se conecta a um serviço NTRIP Caster via protocolo HTTPS (TLS), baixa dados de correção diferencial GPS/GNSS (RTCM3) e os retransmite através de uma interface de rádio LoRa com uma camada de ofuscação de dados. 

O dispositivo foi projetado com foco em resiliência de rede e segurança, operando com redundância entre Ethernet e Wi-Fi, além de receber suas configurações iniciais dinamicamente através da nuvem (AWS IoT) via MQTT.

---

## 🚀 Principais Funcionalidades

* **Redundância de Rede (Failover):** Prioriza a conexão via Ethernet (LAN8720). Caso o cabo seja desconectado, o sistema aciona automaticamente a interface Wi-Fi como plano de contingência.
* **Provisionamento via AWS IoT:** Conecta-se à AWS IoT usando MQTT mútuo (certificados embutidos) e escuta o tópico `esp32/startup` para receber credenciais do Caster NTRIP e configurações Iniciais via JSON.
* **Conexão NTRIP Segura (TLS):** Estabelece comunicação criptografada com o servidor NTRIP utilizando a biblioteca `esp_tls`, com suporte a autenticação HTTP Basic (Base64).
* **Processamento e Ofuscação LoRa:** Filtra o stream contínuo aguardando o cabeçalho RTCM3 (`0xD3`), acumula os dados em buffers de 200 bytes e os embaralha (XOR e troca de setores) antes de transmiti-los via rádio LoRa.
* **Interface Bluetooth (NimBLE):** Disponibiliza um servidor BLE GATT simulando um serviço UART (Nordic UART) com características de RX/TX, útil para depuração ou pareamento com aplicativos móveis.

---

## 🛠️ Configurações de Hardware (Pinout)

O código espera as seguintes conexões entre o ESP32 e os periféricos:

### Módulo LoRa (SPI)
| Pino ESP32 | Função LoRa |
| :--- | :--- |
| `14` | SCK |
| `35` | MISO |
| `13` | MOSI |
| `5` | SS / CS |
| `4` | RST |
| `34` | DIO0 |

### Módulo Ethernet (LAN8720)
| Pino ESP32 | Função Ethernet |
| :--- | :--- |
| `23` | MDC |
| `18` | MDIO |
| `N/A` | PHY Address: 1 |
| `N/A` | Clock Mode: EMAC_CLK_OUT_180_GPIO |

---

## 📻 Configurações de Rádio (LoRa)

O driver LoRa customizado inicializa o rádio com os seguintes parâmetros definidos no firmware:

* **Frequência:** 927.8 MHz.
* **Potência de Transmissão:** 17 dBm.
* **Largura de Banda (Bandwidth):** 7 (Equivalente a 125 kHz no chip SX1276).
* **Fator de Espalhamento (Spreading Factor):** 7.
* **Taxa de Codificação (Coding Rate):** 1 (Equivalente a 4/8).
* **Sync Word:** `0xF3`.
* **CRC:** Habilitado.

---

## 📡 Arquitetura de Software e Fluxo de Dados

### 1. Inicialização e Conectividade
A aplicação inicia o barramento LoRa e, em seguida, levanta a pilha de rede. A conexão primária é a Ethernet; o Wi-Fi em modo STA atua como fallback caso não haja IP na interface cabeada em até 8 segundos.

### 2. Requisição de Configurações (AWS IoT)
Uma vez com rede, o cliente MQTT conecta-se à AWS (`a3kzwueok42ssw-ats.iot.us-east-1.amazonaws.com`) utilizando os certificados embutidos em tempo de compilação. Ele espera um payload JSON no tópico `esp32/startup` contendo:
* `casterHost`, `casterPort`
* `casterUser`, `casterUserPW`
* `mountPoint`, `ggaSentence`

### 3. Loop NTRIP (Task dedicada)
A rotina `taskNTRIP` toma controle após receber os dados da AWS. Ela:
1.  Abre um socket TLS (`esp_tls_conn_new_sync`).
2.  Gera o cabeçalho de requisição HTTP GET com autorização Base64.
3.  Envia o comando GGA inicial.
4.  Lê o fluxo de bytes continuamente, aplicando um *watchdog* de 50 segundos para pacotes ociosos. A cada 10 segundos, o firmware reenvia a sentença GGA para manter a sessão ativa com casters VRS.

### 4. Algoritmo de Ofuscação LoRa (`shuffle_sectors`)
Para evitar transmissões em texto claro, o firmware fatia pacotes RTCM de 200 bytes em quatro setores:
* O Setor 0 é trocado de posição com o Setor 2.
* O Setor 1 é trocado com o Setor 3.
* Qualquer byte "sobrante" no final do buffer sofre uma operação XOR simples utilizando a chave hexadecimal `0xAA`.
* *Nota:* Esta é uma ofuscação simétrica e rápida executada diretamente no buffer; aplicar a mesma função na recepção reverterá os dados ao estado original.

---

## ⚙️ Dependências do Sistema

Este código foi desenvolvido para ser compilado sobre o **ESP-IDF** (Espressif IoT Development Framework). Ele faz uso estrito das seguintes bibliotecas nativas:
* `esp_eth`, `esp_wifi`, `esp_netif` (Gerenciamento de Rede).
* `mqtt_client` (Gerenciamento da AWS IoT).
* `esp_tls`, `mbedtls` (Criptografia e comunicação segura).
* `cJSON` (Para *parsing* do payload da nuvem).
* `nimble` (Stack Bluetooth Low Energy de código aberto).
* `driver/spi_master.h` (Comunicação de baixo nível com o rádio LoRa).

---

## ⚠️ Notas para Compilação

Para compilar corretamente este projeto, atente-se às seguintes preparações:
1.  **Credenciais de Wi-Fi e Certificados:** As credenciais de Wi-Fi estão chumbadas provisoriamente no código e precisam ser substituídas pelas suas.
2.  **AWS Root CA & Chaves do Dispositivo:** O sistema exige que os certificados da AWS sejam incluídos no executável. O firmware faz referência aos ponteiros na memória: `_binary_AmazonRootCA1_pem_start`, `_binary_DeviceCertificate_crt_start`, e `_binary_PrivateKey_key_start`. Você precisa injetá-los no projeto através do `CMakeLists.txt` via instrução `target_add_binary_data`.