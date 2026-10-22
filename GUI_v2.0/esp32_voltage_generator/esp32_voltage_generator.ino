/*
Flash command:
~/.local/bin/esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 921600 write_flash -z \
  0x0 esp32_voltage_generator.ino.bootloader.bin \
  0x8000 esp32_voltage_generator.ino.partitions.bin \
  0x10000 esp32_voltage_generator.ino.bin
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// SPI pin definitions
#define NANO_CS_PIN 12 // chip select (pins 5/6 are Bank 1 I2C)
#define RST_PIN 11 // reset
#define RST_ACTIVE_LEVEL HIGH // PSEC6 RESET: pull high, then low (design doc section 6, Clock Test)

// ── I2C bus pin definitions ───────────────────────────────────────────────────
#define BANK0_SDA 3
#define BANK0_SCL 4

#define BANK1_SDA 5
#define BANK1_SCL 6

#define BANK2_SDA 9  // Bit-bang I2C
#define BANK2_SCL 10  // Bit-bang I2C

// ── I2C bus objects (hardware only for banks 0 & 1) ──────────────────────────
TwoWire* const i2cHW[2] = { &Wire, &Wire1 };

// ── DAC70501 I2C addresses ────────────────────────────────────────────────────
const uint8_t DAC_ADDR[4] = { 0x48, 0x49, 0x4A, 0x4B };

// ── DAC70501 registers ────────────────────────────────────────────────────────
const uint8_t REG_SYNC = 0x02;    // 0x01 is the read-only DEVID register
const uint8_t REG_CONFIG = 0x03;  //0x02;
const uint8_t REG_GAIN = 0x04;
const uint8_t REG_TRIGGER = 0x05;
const uint8_t REG_DAC = 0x08;

// ── DAC count ─────────────────────────────────────────────────────────────────
const uint8_t NUM_BANKS = 3;
const uint8_t DACS_PER_BANK = 4;
const uint8_t TOTAL_DACS = NUM_BANKS * DACS_PER_BANK;  // 12

// =============================================================================
//  Bit-bang I2C for Bank 2  (no library, ESP32-safe)
// =============================================================================

#define SW_HALF_PERIOD_US 5  // 5 µs half-period → ~100 kHz

#define SDA_HIGH() pinMode(BANK2_SDA, INPUT)
#define SDA_LOW() \
  do { \
    digitalWrite(BANK2_SDA, LOW); \
    pinMode(BANK2_SDA, OUTPUT); \
  } while (0)
#define SCL_HIGH() pinMode(BANK2_SCL, INPUT)
#define SCL_LOW() \
  do { \
    digitalWrite(BANK2_SCL, LOW); \
    pinMode(BANK2_SCL, OUTPUT); \
  } while (0)
#define SDA_READ() digitalRead(BANK2_SDA)

void sw_init() { // serial writing initialization
  digitalWrite(BANK2_SDA, LOW);
  digitalWrite(BANK2_SCL, LOW);
  SDA_HIGH();
  SCL_HIGH();
}

void sw_start() {
  SDA_HIGH();
  delayMicroseconds(SW_HALF_PERIOD_US);
  SCL_HIGH();
  delayMicroseconds(SW_HALF_PERIOD_US);
  SDA_LOW();
  delayMicroseconds(SW_HALF_PERIOD_US);
  SCL_LOW();
  delayMicroseconds(SW_HALF_PERIOD_US);
}

void sw_stop() {
  SDA_LOW();
  delayMicroseconds(SW_HALF_PERIOD_US);
  SCL_HIGH();
  delayMicroseconds(SW_HALF_PERIOD_US);
  SDA_HIGH();
  delayMicroseconds(SW_HALF_PERIOD_US);
}

bool sw_writeByte(uint8_t data) {
  for (int i = 7; i >= 0; i--) {
    if (data & (1 << i)) {
      SDA_HIGH();
    } else {
      SDA_LOW();
    }
    delayMicroseconds(SW_HALF_PERIOD_US);
    SCL_HIGH();
    delayMicroseconds(SW_HALF_PERIOD_US);
    SCL_LOW();
    delayMicroseconds(SW_HALF_PERIOD_US);
  }
  SDA_HIGH();
  delayMicroseconds(SW_HALF_PERIOD_US);
  SCL_HIGH();
  delayMicroseconds(SW_HALF_PERIOD_US);
  bool ack = (SDA_READ() == LOW);
  SCL_LOW();
  delayMicroseconds(SW_HALF_PERIOD_US);
  return ack;
}

bool sw_writeReg(uint8_t addr, uint8_t reg, uint16_t value) {
  sw_start();
  bool ok = true; // ok will be false if one of the following lines fails
  ok &= sw_writeByte((addr << 1) | 0x00); // address is 7bits, but we received 8 bits, so must shift left 1 bit
  // LSB is filled with 0. it's the R/W bit. 0=write, 1=read
  // | 0x00 puts a write bit, 0x01 puts a read bit
  // we don't actually need 0x00, it's redudant here
  ok &= sw_writeByte(reg);
  ok &= sw_writeByte((uint8_t)(value >> 8)); // writes the high half (most significant 8 bits)
  ok &= sw_writeByte((uint8_t)(value & 0xFF)); // writes the low half (least sig 8 bits)
  sw_stop();
  return ok;
}

bool sw_probe(uint8_t addr) {
  sw_start();
  bool ack = sw_writeByte((addr << 1) | 0x00);
  sw_stop();
  return ack;
}

// =============================================================================
//  Unified low-level I2C write (all 3 banks)
// =============================================================================

bool dacWriteReg(uint8_t bank, uint8_t chip, uint8_t reg, uint16_t value) {
  if (bank == 2) {
    return sw_writeReg(DAC_ADDR[chip], reg, value);
  } else {
    TwoWire* bus = i2cHW[bank];
    bus->beginTransmission(DAC_ADDR[chip]);
    bus->write(reg);
    bus->write((uint8_t)(value >> 8));
    bus->write((uint8_t)(value & 0xFF));
    return (bus->endTransmission() == 0);
  }
}

// =============================================================================
//  Chip initialisation & Voltage Helpers
// =============================================================================

bool dacInit(uint8_t bank, uint8_t chip) {
  bool ok = true;
  ok &= dacWriteReg(bank, chip, REG_TRIGGER, 0x000A);
  delay(1);
  ok &= dacWriteReg(bank, chip, REG_SYNC, 0x0000);
  ok &= dacWriteReg(bank, chip, REG_CONFIG, 0x0000);
  ok &= dacWriteReg(bank, chip, REG_GAIN, 0x0001);
  ok &= dacWriteReg(bank, chip, REG_DAC, 0x0000);
  return ok;
}

float codeToVoltage(uint16_t code) {
  return ((float)code / 65536.0f) * 5.0f;
}

// =============================================================================
//  Setup
// =============================================================================

void setup() {
  Serial.begin(115200);

  // Initialize the custom CS pin as an output
  pinMode(NANO_CS_PIN, OUTPUT);

  // SPI CS lines are active LOW; keep it HIGH to start deselected
  digitalWrite(NANO_CS_PIN, HIGH);

  // Reset line idles inactive
  pinMode(RST_PIN, OUTPUT);
  digitalWrite(RST_PIN, !RST_ACTIVE_LEVEL);

  // Initialize the main SPI bus
  SPI.begin();

  // Power on the TFT rail and backlight
  pinMode(TFT_I2C_POWER, OUTPUT);
  digitalWrite(TFT_I2C_POWER, HIGH);
  delay(10);

  pinMode(TFT_BACKLITE, OUTPUT);
  digitalWrite(TFT_BACKLITE, HIGH);

  // Init display: 135x240, landscape
  tft.init(135, 240);
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);

  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(0, 0);
  tft.println("Initializing...");

  // Hardware I2C buses
  Wire.begin(BANK0_SDA, BANK0_SCL);
  Wire1.begin(BANK1_SDA, BANK1_SCL);
  Wire.setClock(400000UL);
  Wire1.setClock(400000UL);

  // Bit-bang I2C bus (Bank 2)
  sw_init();

  // Scan and init all DACs silently
  for (uint8_t bank = 0; bank < NUM_BANKS; bank++) {
    for (uint8_t chip = 0; chip < DACS_PER_BANK; chip++) {
      if (bank == 2) {
        if (sw_probe(DAC_ADDR[chip])) dacInit(bank, chip);
      } else {
        i2cHW[bank]->beginTransmission(DAC_ADDR[chip]);
        if (i2cHW[bank]->endTransmission() == 0) {
          dacInit(bank, chip);
        }
      }
    }
  }

  tft.fillScreen(ST77XX_BLACK);
  tft.setCursor(0, 0);
  tft.println("Ready to receive.");
}

// =============================================================================
//  SPI communication
// =============================================================================
const uint8_t SPI_MAX_BYTES = 8;

// Sends `count` bytes in one chip-select assertion and prints each byte shifted
// back as "RX: XX" for the GUI.
void spiTransfer(const uint8_t *data, uint8_t count) {
  // Send data at 1MHz, MSBFIRST, SPI_MODE0
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(NANO_CS_PIN, LOW);

  for (uint8_t i = 0; i < count; i++) {
    uint8_t rx = SPI.transfer(data[i]);
    Serial.print("RX: ");
    Serial.println(rx, HEX); // newline after each byte
  }

  digitalWrite(NANO_CS_PIN, HIGH);
  SPI.endTransaction();
}

// Pulses the chip RESET pin
void pulseReset() {
  digitalWrite(RST_PIN, RST_ACTIVE_LEVEL);
  delay(1);
  digitalWrite(RST_PIN, !RST_ACTIVE_LEVEL);
}

// =============================================================================
//  Command handling
// =============================================================================
// Every GUI command starts with a command byte:
//   CMD_DAC: 00 bank dac_addr reg value_hi value_lo   (6 bytes)
//   CMD_RST: 01 00                                    (2 bytes)
//   CMD_SPI: 02 count byte0 .. byte(count-1)          (2 + count bytes)
const uint8_t CMD_DAC = 0x00;
const uint8_t CMD_RST = 0x01;
const uint8_t CMD_SPI = 0x02;

void showDacUpdate(uint8_t channel, uint8_t reg, uint16_t code) {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(3);

  // Row 1: DAC number, plus ON/OFF when this was a power-down write
  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(0, 20);
  tft.print("DAC: ");
  tft.setTextColor(ST77XX_WHITE);
  tft.print(channel);

  if (reg == REG_CONFIG) {
    if (code & 0x0001) {
      tft.setTextColor(ST77XX_RED);
      tft.println(" OFF");
    } else {
      tft.setTextColor(ST77XX_GREEN);
      tft.println(" ON");
    }
  }

  // Row 2: Voltage (only meaningful for a DAC data write)
  if (reg == REG_DAC) {
    tft.setTextColor(ST77XX_CYAN);
    tft.setCursor(0, 70);
    tft.println("VOLTAGE:");
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(0, 100);
    tft.print(codeToVoltage(code), 4);
    tft.print(" V");
  }
}

void handleDacCommand(const uint8_t *msg) {
  uint8_t bank = msg[1];
  uint8_t addr = msg[2];
  uint8_t reg = msg[3];
  uint16_t code = ((uint16_t)msg[4] << 8) | msg[5];

  uint8_t chip = addr - DAC_ADDR[0];
  if (bank >= NUM_BANKS || addr < DAC_ADDR[0] || chip >= DACS_PER_BANK) return; // malformed
  if (reg != REG_SYNC && reg != REG_CONFIG && reg != REG_GAIN && reg != REG_TRIGGER && reg != REG_DAC) return;

  dacWriteReg(bank, chip, reg, code);
  showDacUpdate(bank * DACS_PER_BANK + chip, reg, code);
}

void handleSpiCommand(const uint8_t *msg) {
  spiTransfer(&msg[2], msg[1]);
}

const uint8_t MAX_MSG_LEN = 2 + SPI_MAX_BYTES;

void loop() {
  static uint8_t msg[MAX_MSG_LEN];
  static uint8_t have = 0; // bytes of the current message received so far
  static uint8_t need = 0; // total length of the current message (0 = not known yet)

  while (Serial.available()) {
    uint8_t b = Serial.read();

    if (have == 0) {
      if (b == CMD_DAC) need = 6;
      else if (b == CMD_RST) need = 2;
      else if (b == CMD_SPI) need = 0; // length comes from the count byte
      else continue; // not a command byte, drop it to resynchronize
    }
    msg[have++] = b;

    if (have == 2 && msg[0] == CMD_SPI) {
      if (b == 0 || b > SPI_MAX_BYTES) { // bad count, drop the message
        have = 0;
        continue;
      }
      need = 2 + b;
    }

    if (need != 0 && have == need) {
      if (msg[0] == CMD_DAC) handleDacCommand(msg);
      else if (msg[0] == CMD_RST) pulseReset();
      else handleSpiCommand(msg);
      have = 0;
      need = 0;
    }
  }
}
