# Gateway LoRaWAN 2 Canais

Gateway LoRaWAN dual-channel baseado em ESP32 + 2x SX1276, permitindo recepção e transmissão simultâneas (pseudo full-duplex) para não perder pacotes durante operações de downlink.

## Caracteristicas

- **Dual Radio**: Dois módulos SX1276 operando independentemente
  - Radio 0: RX contínuo (nunca perde pacotes)
  - Radio 1: TX sob demanda (downlinks)
- **Frequências AU915**: Compatível com regulamentação brasileira
  - Sub-bandas configuráveis (1-8)
  - Padrão: Sub-banda 2 (TTN Brasil)
- **Conectividade**: WiFi + Ethernet (W5500) com failover automático
- **Protocolo**: Semtech UDP Packet Forwarder (porta 1700)
- **Servidores suportados**: TTN, ChirpStack, e outros compatíveis

## Hardware Necessário

- ESP32 NodeMCU (38 pinos)
- 2x Módulos SX1276 (RFM95W, Ra-02, ou similar)
- 2x Antenas 915MHz
- 1x Módulo W5500 (opcional, para Ethernet)

## Pinagem

| Função     | SX1276 #1 (RX) | SX1276 #2 (TX) | W5500    |
|------------|----------------|----------------|----------|
| SPI MOSI   | GPIO23         | GPIO23         | GPIO23   |
| SPI MISO   | GPIO19         | GPIO19         | GPIO19   |
| SPI SCK    | GPIO18         | GPIO18         | GPIO18   |
| NSS (CS)   | GPIO5          | GPIO4          | GPIO15   |
| RESET      | GPIO14         | GPIO27         | GPIO33   |
| DIO0 (IRQ) | GPIO26         | GPIO25         | -        |
| DIO1       | GPIO35         | GPIO34         | -        |
| INT        | -              | -              | GPIO36   |

Veja [docs/wiring.md](docs/wiring.md) para o diagrama completo.

## Compilação

### Pré-requisitos

- ESP-IDF v5.0 ou superior
- Python 3.x

### Build

```bash
# Configure o ambiente ESP-IDF
. $IDF_PATH/export.sh

# Configure o projeto (opcional)
idf.py menuconfig

# Compile
idf.py build

# Grave no ESP32
idf.py -p /dev/ttyUSB0 flash monitor
```

## Configuração

### Via menuconfig

```bash
idf.py menuconfig
```

Navegue até "Gateway LoRaWAN 2CH Configuration" para configurar:

- **LoRa Radio Configuration**: Frequência, SF, BW, potência TX
- **SX1276 Pin Configuration**: Pinos dos módulos LoRa
- **W5500 Ethernet Configuration**: Pinos e habilitação
- **WiFi Configuration**: SSID e senha
- **LoRaWAN Server Configuration**: Servidor, porta, Gateway EUI

### Frequências AU915

| Sub-banda | Canais | Frequências (MHz)       |
|-----------|--------|-------------------------|
| SB1       | 0-7    | 915.2 - 916.6           |
| SB2 (TTN) | 8-15   | 916.8 - 918.2           |
| SB3       | 16-23  | 918.4 - 919.8           |
| SB4       | 24-31  | 920.0 - 921.4           |
| ...       | ...    | ...                     |

## Arquitetura

```
┌─────────────────────────────────────────────────────────┐
│                      ESP32                              │
│  ┌──────────────┐              ┌──────────────┐        │
│  │   SX1276 #1  │              │   SX1276 #2  │        │
│  │   (RX Only)  │              │   (TX Only)  │        │
│  │ RX Contínuo  │              │ TX on Demand │        │
│  └──────┬───────┘              └──────┬───────┘        │
│         ▼                             ▲                │
│  ┌──────────────────────────────────────────┐         │
│  │           Gateway Core Task              │         │
│  └──────────────────┬───────────────────────┘         │
│                     ▼                                  │
│  ┌──────────────────────────────────────────┐         │
│  │         Packet Forwarder (UDP)           │         │
│  └──────────────────┬───────────────────────┘         │
│         ┌───────────┴───────────┐                     │
│         ▼                       ▼                     │
│    ┌─────────┐            ┌──────────┐               │
│    │  WiFi   │            │ Ethernet │               │
│    └────┬────┘            └────┬─────┘               │
└─────────┼──────────────────────┼─────────────────────┘
          └──────────┬───────────┘
                     ▼
          ┌─────────────────────┐
          │  Servidor LoRaWAN   │
          │  (TTN / ChirpStack) │
          └─────────────────────┘
```

## Estrutura do Projeto

```
GatewayLora2Ch/
├── CMakeLists.txt
├── sdkconfig.defaults
├── partitions.csv
├── main/
│   ├── main.c              # Aplicação principal
│   └── Kconfig.projbuild   # Opções de configuração
├── components/
│   ├── sx1276/             # Driver SX1276
│   ├── lora_gateway/       # Core do gateway
│   ├── network/            # WiFi + Ethernet
│   └── config/             # Configurações NVS
└── docs/
    └── wiring.md           # Diagrama de conexões
```

## Protocolo Semtech UDP

O gateway implementa o protocolo Semtech UDP (porta 1700):

| Tipo       | Código | Descrição                    |
|------------|--------|------------------------------|
| PUSH_DATA  | 0x00   | Gateway → Server (uplink)    |
| PUSH_ACK   | 0x01   | Server → Gateway (ack)       |
| PULL_DATA  | 0x02   | Gateway → Server (keepalive) |
| PULL_RESP  | 0x03   | Server → Gateway (downlink)  |
| PULL_ACK   | 0x04   | Server → Gateway (ack)       |
| TX_ACK     | 0x05   | Gateway → Server (tx result) |

## Monitoramento

O gateway envia estatísticas a cada 30 segundos:
- Pacotes recebidos (total, ok, bad)
- Pacotes transmitidos
- Status de conexão

Via monitor serial:
```
I (xxx) main: === Gateway Status ===
I (xxx) main: Uptime: 3600 s
I (xxx) main: RX: total=150, ok=148, bad=2
I (xxx) main: TX: total=12, ok=12, fail=0
I (xxx) main: Network: Connected
I (xxx) main: Server: Connected
```

## Troubleshooting

### SX1276 não detectado
- Verifique conexões SPI (MOSI, MISO, SCK)
- Verifique alimentação 3.3V
- Verifique pino CS do módulo

### Não conecta ao servidor
- Verifique configuração WiFi/Ethernet
- Verifique se o servidor está acessível
- Verifique Gateway EUI

### Não recebe pacotes
- Verifique se as antenas estão conectadas
- Verifique configuração de frequência/sub-banda
- Verifique Sync Word (0x34 para LoRaWAN público)

## Licença

MIT License - Veja [LICENSE](LICENSE)
