# Gateway LoRaWAN 2CH - Diagrama de Conexões

## Componentes Necessários

- 1x ESP32 NodeMCU (38 pinos)
- 2x Módulos SX1276 (RFM95W, Ra-02, ou similar)
- 1x Módulo Ethernet W5500 (opcional)
- Antenas 915MHz (2x)
- Fonte de alimentação 3.3V

## Pinagem

### Barramento SPI (Compartilhado)

| Função | ESP32 GPIO |
|--------|------------|
| MOSI   | GPIO23     |
| MISO   | GPIO19     |
| SCK    | GPIO18     |

### SX1276 #1 (Rádio RX)

| Função      | ESP32 GPIO | Pino SX1276 |
|-------------|------------|-------------|
| NSS (CS)    | GPIO5      | NSS         |
| RESET       | GPIO14     | RESET       |
| DIO0 (IRQ)  | GPIO26     | DIO0        |
| DIO1        | GPIO35     | DIO1        |
| DIO2        | GPIO32     | DIO2        |
| VCC         | 3.3V       | VCC         |
| GND         | GND        | GND         |

### SX1276 #2 (Rádio TX)

| Função      | ESP32 GPIO | Pino SX1276 |
|-------------|------------|-------------|
| NSS (CS)    | GPIO4      | NSS         |
| RESET       | GPIO27     | RESET       |
| DIO0 (IRQ)  | GPIO25     | DIO0        |
| DIO1        | GPIO34     | DIO1        |
| DIO2        | GPIO33     | DIO2        |
| VCC         | 3.3V       | VCC         |
| GND         | GND        | GND         |

### W5500 Ethernet (Opcional)

| Função   | ESP32 GPIO | Pino W5500 |
|----------|------------|------------|
| NSS (CS) | GPIO15     | SCS        |
| RESET    | GPIO33     | RST        |
| INT      | GPIO36     | INT        |
| VCC      | 3.3V       | VCC        |
| GND      | GND        | GND        |

## Diagrama de Conexões

```
                           ┌──────────────────┐
                           │    ESP32 NodeMCU │
                           │                  │
    ┌──────────┐           │  GPIO23 ─────────┼──────────┬───────────────┐
    │ SX1276   │           │  GPIO19 ─────────┼──────────┤               │
    │ Radio #1 │           │  GPIO18 ─────────┼──────────┤  SPI Bus      │
    │   (RX)   │           │                  │          │               │
    │          │           │  GPIO5  ─────────┼───► NSS  │               │
    │  NSS  ◄──┼───────────┤                  │          │               │
    │  RST  ◄──┼───────────┤  GPIO14 ─────────┼───► RST  │               │
    │  DIO0 ──►┼───────────┤  GPIO26 ─────────┼◄── IRQ   │               │
    │  DIO1 ──►┼───────────┤  GPIO35 ◄────────┤          │    ┌──────────┤
    │  DIO2 ──►┼───────────┤  GPIO32 ◄────────┤          │    │ SX1276   │
    └──────────┘           │                  │          │    │ Radio #2 │
                           │  GPIO4  ─────────┼───► NSS ─┼───►│   (TX)   │
    ┌──────────┐           │  GPIO27 ─────────┼───► RST ─┼───►│          │
    │  W5500   │           │  GPIO25 ─────────┼◄── IRQ ──┼────┤  DIO0    │
    │ Ethernet │           │  GPIO34 ◄────────┼──────────┼────┤  DIO1    │
    │(Opcional)│           │  GPIO33 ◄────────┼──────────┼────┤  DIO2    │
    │          │           │                  │          │    └──────────┘
    │  SCS  ◄──┼───────────┤  GPIO15 ─────────┤          │
    │  RST  ◄──┼───────────┤  GPIO33 ─────────┤          │
    │  INT  ──►┼───────────┤  GPIO36 ◄────────┤          │
    └──────────┘           │                  │          │
                           │      3.3V ───────┼──────────┴── VCC (todos)
                           │      GND  ───────┼───────────── GND (todos)
                           │                  │
                           └──────────────────┘
```

## Notas Importantes

### Alimentação
- Use uma fonte de 3.3V estável
- Capacitores de desacoplamento (100nF) próximos a cada módulo SX1276
- Os módulos LoRa podem consumir até 120mA durante TX

### Antenas
- **NUNCA** ligue os módulos SX1276 sem antenas conectadas
- Use antenas adequadas para 915MHz
- Mantenha as antenas afastadas pelo menos 20cm uma da outra

### Interferência
- Os dois rádios operam em frequências próximas
- O rádio RX usa sub-banda de uplink (916.8-918.2 MHz para SB2)
- O rádio TX usa frequências de downlink (923.3-927.5 MHz)
- A separação de frequência minimiza interferência

### Pinos de Entrada
- GPIO34, GPIO35, GPIO36 são apenas entrada no ESP32
- Usados para DIO1/DIO2 que são sinais de saída dos SX1276

## Configuração de Fábrica

### Sub-banda AU915 Padrão (TTN Brasil)
- **Sub-banda 2**: Canais 8-15
- Frequências de uplink: 916.8, 917.0, 917.2, 917.4, 917.6, 917.8, 918.0, 918.2 MHz
- Frequência de downlink: 923.3 MHz (RX1), 923.3 MHz (RX2 com SF12)

### Parâmetros LoRa
- Sync Word: 0x34 (LoRaWAN público)
- Spreading Factor: SF7-SF10 (uplink), SF12 (RX2)
- Bandwidth: 125 kHz (uplink), 500 kHz (downlink)
- Coding Rate: 4/5

## Teste de Conexão

1. Conecte os módulos conforme diagrama
2. Compile e grave o firmware
3. Abra o monitor serial (115200 baud)
4. Verifique as mensagens de inicialização:

```
I (xxx) sx1276: SX1276 detected, version: 0x12
I (xxx) lora_gw: RX Radio version: 0x12
I (xxx) lora_gw: TX Radio version: 0x12
```

Se a versão do chip não aparecer ou for diferente de 0x12:
- Verifique as conexões SPI
- Verifique a alimentação 3.3V
- Verifique os pinos CS de cada módulo
