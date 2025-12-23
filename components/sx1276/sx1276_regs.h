/**
 * @file sx1276_regs.h
 * @brief SX1276 Register Definitions
 */

#ifndef SX1276_REGS_H
#define SX1276_REGS_H

// Common registers
#define REG_FIFO                    0x00
#define REG_OP_MODE                 0x01
#define REG_FRF_MSB                 0x06
#define REG_FRF_MID                 0x07
#define REG_FRF_LSB                 0x08
#define REG_PA_CONFIG               0x09
#define REG_PA_RAMP                 0x0A
#define REG_OCP                     0x0B
#define REG_LNA                     0x0C
#define REG_FIFO_ADDR_PTR           0x0D
#define REG_FIFO_TX_BASE_ADDR       0x0E
#define REG_FIFO_RX_BASE_ADDR       0x0F
#define REG_FIFO_RX_CURRENT_ADDR    0x10
#define REG_IRQ_FLAGS_MASK          0x11
#define REG_IRQ_FLAGS               0x12
#define REG_RX_NB_BYTES             0x13
#define REG_RX_HEADER_CNT_MSB       0x14
#define REG_RX_HEADER_CNT_LSB       0x15
#define REG_RX_PACKET_CNT_MSB       0x16
#define REG_RX_PACKET_CNT_LSB       0x17
#define REG_MODEM_STAT              0x18
#define REG_PKT_SNR_VALUE           0x19
#define REG_PKT_RSSI_VALUE          0x1A
#define REG_RSSI_VALUE              0x1B
#define REG_HOP_CHANNEL             0x1C
#define REG_MODEM_CONFIG_1          0x1D
#define REG_MODEM_CONFIG_2          0x1E
#define REG_SYMB_TIMEOUT_LSB        0x1F
#define REG_PREAMBLE_MSB            0x20
#define REG_PREAMBLE_LSB            0x21
#define REG_PAYLOAD_LENGTH          0x22
#define REG_MAX_PAYLOAD_LENGTH      0x23
#define REG_HOP_PERIOD              0x24
#define REG_FIFO_RX_BYTE_ADDR       0x25
#define REG_MODEM_CONFIG_3          0x26
#define REG_PPM_CORRECTION          0x27
#define REG_FEI_MSB                 0x28
#define REG_FEI_MID                 0x29
#define REG_FEI_LSB                 0x2A
#define REG_RSSI_WIDEBAND           0x2C
#define REG_DETECT_OPTIMIZE         0x31
#define REG_INVERT_IQ               0x33
#define REG_DETECTION_THRESHOLD     0x37
#define REG_SYNC_WORD               0x39
#define REG_INVERT_IQ_2             0x3B
#define REG_DIO_MAPPING_1           0x40
#define REG_DIO_MAPPING_2           0x41
#define REG_VERSION                 0x42
#define REG_TCXO                    0x4B
#define REG_PA_DAC                  0x4D
#define REG_FORMER_TEMP             0x5B
#define REG_AGC_REF                 0x61
#define REG_AGC_THRESH_1            0x62
#define REG_AGC_THRESH_2            0x63
#define REG_AGC_THRESH_3            0x64
#define REG_PLL                     0x70

// Operating modes (REG_OP_MODE)
#define MODE_LONG_RANGE_MODE        0x80
#define MODE_ACCESS_SHARED_REG      0x40
#define MODE_LOW_FREQUENCY_MODE     0x08
#define MODE_SLEEP                  0x00
#define MODE_STDBY                  0x01
#define MODE_FSTX                   0x02
#define MODE_TX                     0x03
#define MODE_FSRX                   0x04
#define MODE_RX_CONTINUOUS          0x05
#define MODE_RX_SINGLE              0x06
#define MODE_CAD                    0x07

// PA configuration (REG_PA_CONFIG)
#define PA_BOOST                    0x80

// IRQ masks (REG_IRQ_FLAGS)
#define IRQ_CAD_DETECTED            0x01
#define IRQ_FHSS_CHANGE_CHANNEL     0x02
#define IRQ_CAD_DONE                0x04
#define IRQ_TX_DONE                 0x08
#define IRQ_VALID_HEADER            0x10
#define IRQ_PAYLOAD_CRC_ERROR       0x20
#define IRQ_RX_DONE                 0x40
#define IRQ_RX_TIMEOUT              0x80

// Modem config 1 (REG_MODEM_CONFIG_1)
#define MODEM_CONFIG1_BW_MASK       0xF0
#define MODEM_CONFIG1_CR_MASK       0x0E
#define MODEM_CONFIG1_IMPLICIT_HEADER 0x01

// Bandwidth values
#define BW_7_8_KHZ                  0x00
#define BW_10_4_KHZ                 0x10
#define BW_15_6_KHZ                 0x20
#define BW_20_8_KHZ                 0x30
#define BW_31_25_KHZ                0x40
#define BW_41_7_KHZ                 0x50
#define BW_62_5_KHZ                 0x60
#define BW_125_KHZ                  0x70
#define BW_250_KHZ                  0x80
#define BW_500_KHZ                  0x90

// Coding rates
#define CR_4_5                      0x02
#define CR_4_6                      0x04
#define CR_4_7                      0x06
#define CR_4_8                      0x08

// Modem config 2 (REG_MODEM_CONFIG_2)
#define MODEM_CONFIG2_SF_MASK       0xF0
#define MODEM_CONFIG2_TX_CONT       0x08
#define MODEM_CONFIG2_RX_CRC        0x04

// Spreading factors
#define SF_6                        0x60
#define SF_7                        0x70
#define SF_8                        0x80
#define SF_9                        0x90
#define SF_10                       0xA0
#define SF_11                       0xB0
#define SF_12                       0xC0

// DIO mapping (REG_DIO_MAPPING_1)
#define DIO0_RX_DONE                0x00
#define DIO0_TX_DONE                0x40
#define DIO0_CAD_DONE               0x80
#define DIO1_RX_TIMEOUT             0x00
#define DIO1_FHSS_CHANGE            0x10
#define DIO1_CAD_DETECTED           0x20
#define DIO2_FHSS_CHANGE            0x00
#define DIO3_CAD_DONE               0x00
#define DIO3_VALID_HEADER           0x01
#define DIO3_PAYLOAD_CRC_ERROR      0x02

// Detection optimize
#define DETECT_OPTIMIZE_SF7_12      0x03
#define DETECT_OPTIMIZE_SF6         0x05

// Detection threshold
#define DETECTION_THRESHOLD_SF7_12  0x0A
#define DETECTION_THRESHOLD_SF6     0x0C

// LoRaWAN sync word
#define LORA_MAC_PUBLIC_SYNCWORD    0x34
#define LORA_MAC_PRIVATE_SYNCWORD   0x12

// Expected chip version
#define SX1276_VERSION              0x12

#endif // SX1276_REGS_H
