#pragma once

/*
  MAYAP HMI ST7567S 128x64 + rotary + buzzer - v3.7.0
  Phan cung: LCD 0x3F SDA8/SCL9, rotary 38/39/40, buzzer GPIO41.
  File nay chi dung trong firmware tong; khong chua setup/loop demo, Wi-Fi,
  ket noi mang, luu flash noi hay dieu khien GPIO chap hanh.
*/

#include "config.h"
#include "boot_diagnostic.h"
#include "boot_assets.h"
#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <math.h>
#include <stddef.h>
#include <string.h>
#include <type_traits>
#include <ctype.h>
#include <stdlib.h>
#if MAYAP_HMI_ENCODER_INTERRUPT
#include <driver/gpio.h>
#include "gpio_interrupts.h"
#include <hal/gpio_ll.h>
#include <soc/gpio_struct.h>
#endif

#if LCD_PROFILE == 1
U8G2_ST7567_ENH_DG128064I_F_HW_I2C lcd(U8G2_R0, U8X8_PIN_NONE);
#elif LCD_PROFILE == 2
U8G2_ST7567_JLX12864_F_HW_I2C lcd(U8G2_R0, U8X8_PIN_NONE);
#else
#error "LCD_PROFILE khong hop le"
#endif

// ============================================================================
// QR CODE (Version 1, EC level Q, che do Alphanumeric, mask co dinh = 0) -
// CHI ma hoa dung dinh dang Device ID "MAP-XXXXXXXXXXXX" (16 ky tu), khong
// phai bo ma hoa QR tong quat. Man LCD 128x64 mono qua nho de ve QR "day du"
// (nhieu phien ban/muc do loi hon) van con doc duoc - Version 1 la kich
// thuoc NHO NHAT chuan QR cho phep (21x21 o), toi da hoa so pixel/o tren
// man hinh chat hep nay. EC level Q (30% du thua, khong phai muc cao nhat
// H) la muc CAO NHAT van du cho du 16 ky tu ID nam trong 1 Version 1 -
// dung H se buoc phai nhay len Version 2 (25x25), lam pixel/o NHO DI, phan
// tac dung. Mask co dinh = 0 (bo qua buoc "chon mask toi uu" ma cac bo ma
// hoa QR day du hay lam) vi ket qua van hop chuan/quet duoc binh thuong,
// chi khac ve tham my phan bo diem den/trang, doi lai giam dang ke code
// (khong can tinh diem phat 8 mask).
//
// Thuat toan da duoc KIEM CHUNG rieng bang Python + pyzbar (thu vien giai
// ma doc lap, khong lien quan gi code o day) truoc khi dich sang C++: 34
// ID ngau nhien (gom ca bien MAP-000000000000/MAP-FFFFFFFFFFFF) deu quet
// ra dung chuoi goc. Sai du chi 1 bit o day la QR khong quet duoc ma
// khong co canh bao gi khi bien dich hay chay thu - nen giu dung cong thuc
// da kiem chung, KHONG "toi uu lai" cho gon hon neu khong kiem chung lai.
namespace QrMapId {
constexpr uint8_t SIZE = 21;
constexpr const char *ALPHANUM = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";
// Format info 15-bit (EC=Q, mask=0) la HANG SO co dinh, khong phu thuoc du
// lieu - tinh san bang BCH thay vi tinh lai moi lan.
constexpr uint16_t FORMAT_BITS = 0b011010101011111;
// Da thuc sinh (generator polynomial) cho 13 codeword sua loi - CO DINH cho
// moi truong hop 13 EC codeword, khong phu thuoc du lieu dau vao.
constexpr uint8_t GEN_POLY[14] = {1, 137, 73, 227, 17, 177, 17, 52, 13, 46, 43, 83, 132, 120};

// Bang GF(256) cho phep nhan Reed-Solomon (da thuc nguyen thuy QR chuan
// x^8+x^4+x^3+x^2+1 = 0x11D) - tinh san luc build thay vi tinh runtime.
constexpr uint8_t GF_EXP[256] = {
  1,2,4,8,16,32,64,128,29,58,116,232,205,135,19,38,76,152,45,90,180,117,234,201,
  143,3,6,12,24,48,96,192,157,39,78,156,37,74,148,53,106,212,181,119,238,193,
  159,35,70,140,5,10,20,40,80,160,93,186,105,210,185,111,222,161,95,190,97,194,
  153,47,94,188,101,202,137,15,30,60,120,240,253,231,211,187,107,214,177,127,
  254,225,223,163,91,182,113,226,217,175,67,134,17,34,68,136,13,26,52,104,208,
  189,103,206,129,31,62,124,248,237,199,147,59,118,236,197,151,51,102,204,133,
  23,46,92,184,109,218,169,79,158,33,66,132,21,42,84,168,77,154,41,82,164,85,
  170,73,146,57,114,228,213,183,115,230,209,191,99,198,145,63,126,252,229,215,
  179,123,246,241,255,227,219,171,75,150,49,98,196,149,55,110,220,165,87,174,
  65,130,25,50,100,200,141,7,14,28,56,112,224,221,167,83,166,81,162,89,178,121,
  242,249,239,195,155,43,86,172,69,138,9,18,36,72,144,61,122,244,245,247,243,
  251,235,203,139,11,22,44,88,176,125,250,233,207,131,27,54,108,216,173,71,142,1
};
constexpr uint8_t GF_LOG[256] = {
  0,0,1,25,2,50,26,198,3,223,51,238,27,104,199,75,4,100,224,14,52,141,239,129,
  28,193,105,248,200,8,76,113,5,138,101,47,225,36,15,33,53,147,142,218,240,18,
  130,69,29,181,194,125,106,39,249,185,201,154,9,120,77,228,114,166,6,191,139,
  98,102,221,48,253,226,152,37,179,16,145,34,136,54,208,148,206,143,150,219,
  189,241,210,19,92,131,56,70,64,30,66,182,163,195,72,126,110,107,58,40,84,250,
  133,186,61,202,94,155,159,10,21,121,43,78,212,229,172,115,243,167,87,7,112,
  192,247,140,128,99,13,103,74,222,237,49,197,254,24,227,165,153,119,38,184,
  180,124,17,68,146,217,35,32,137,46,55,63,209,91,149,188,207,205,144,135,151,
  178,220,252,190,97,242,86,211,171,20,42,93,158,132,60,57,83,71,109,65,162,31,
  45,67,216,183,123,164,118,196,23,73,236,127,12,111,246,108,161,59,82,41,157,
  85,170,251,96,134,177,187,204,62,90,203,89,95,176,156,169,160,81,11,245,22,
  235,122,117,44,215,79,174,213,233,230,231,173,232,116,214,244,234,168,80,88,175
};

uint8_t gfMul(uint8_t a, uint8_t b) {
  if (a == 0 || b == 0) return 0;
  const uint16_t sum = static_cast<uint16_t>(GF_LOG[a]) + GF_LOG[b];
  return GF_EXP[sum % 255];
}

int8_t alphanumericValue(char c) {
  const char *p = strchr(ALPHANUM, c);
  return p ? static_cast<int8_t>(p - ALPHANUM) : -1;
}

// text PHAI dung 16 ky tu alphanumeric hop le (dinh dang Device ID). Tra ve
// false neu khong dung dinh dang - goi noi dung KHONG duoc ve QR sai.
bool buildDataCodewords(const char *text, uint8_t out[13]) {
  if (!text || strlen(text) != 16U) return false;
  bool bits[104] = {false};
  int pos = 0;
  auto pushBits = [&](uint32_t value, int count) {
    for (int i = count - 1; i >= 0; --i) bits[pos++] = (value >> i) & 1U;
  };
  pushBits(0b0010U, 4);   // mode indicator: Alphanumeric
  pushBits(16U, 9);       // character count indicator (V1, alphanumeric = 9 bit)
  for (int i = 0; i < 16; i += 2) {
    const int8_t v1 = alphanumericValue(text[i]);
    const int8_t v2 = alphanumericValue(text[i + 1]);
    if (v1 < 0 || v2 < 0) return false;
    pushBits(static_cast<uint32_t>(v1) * 45U + static_cast<uint32_t>(v2), 11);
  }
  // 4+9+8*11 = 101 bit dung 16 ky tu; 3 bit terminator con lai da la 0 san
  // trong mang (khoi tao {false}) - vua khop 104 bit = 13 byte, khong can
  // pad codeword them.
  if (pos != 101) return false;
  for (int i = 0; i < 13; ++i) {
    uint8_t byte = 0;
    for (int b = 0; b < 8; ++b) byte = static_cast<uint8_t>((byte << 1) | (bits[i * 8 + b] ? 1 : 0));
    out[i] = byte;
  }
  return true;
}

void rsEcCodewords(const uint8_t data[13], uint8_t out[13]) {
  uint8_t remainder[26] = {0};
  for (int i = 0; i < 13; ++i) remainder[i] = data[i];
  for (int i = 0; i < 13; ++i) {
    const uint8_t coeff = remainder[i];
    if (coeff == 0) continue;
    for (int j = 0; j < 14; ++j) {
      remainder[i + j] ^= gfMul(GEN_POLY[j], coeff);
    }
  }
  for (int i = 0; i < 13; ++i) out[i] = remainder[13 + i];
}

bool isFunctionModule(int8_t r, int8_t c) {
  if (r < 9 && c < 9) return true;
  if (r < 9 && c >= SIZE - 8) return true;
  if (r >= SIZE - 8 && c < 9) return true;
  if (r == 6 || c == 6) return true;
  return false;
}

void setFinder(bool m[SIZE][SIZE], int8_t top, int8_t left) {
  for (int8_t r = -1; r <= 7; ++r) {
    for (int8_t c = -1; c <= 7; ++c) {
      const int rr = top + r, cc = left + c;
      if (rr < 0 || rr >= SIZE || cc < 0 || cc >= SIZE) continue;
      if (r >= 0 && r <= 6 && c >= 0 && c <= 6) {
        m[rr][cc] = (r == 0 || r == 6 || c == 0 || c == 6 ||
                     (r >= 2 && r <= 4 && c >= 2 && c <= 4));
      } else {
        m[rr][cc] = false;  // vanh trang (separator) quanh finder
      }
    }
  }
}

void buildMatrix(const uint8_t data[13], const uint8_t ec[13], bool m[SIZE][SIZE]) {
  setFinder(m, 0, 0);
  setFinder(m, 0, SIZE - 7);
  setFinder(m, SIZE - 7, 0);
  for (int8_t i = 8; i < SIZE - 8; ++i) {
    m[6][i] = (i % 2 == 0);
    m[i][6] = (i % 2 == 0);
  }
  m[SIZE - 8][8] = true;  // dark module co dinh cua V1

  bool allBits[208];
  int bi = 0;
  for (int k = 0; k < 13; ++k) for (int b = 7; b >= 0; --b) allBits[bi++] = (data[k] >> b) & 1U;
  for (int k = 0; k < 13; ++k) for (int b = 7; b >= 0; --b) allBits[bi++] = (ec[k] >> b) & 1U;

  int bitI = 0;
  int8_t col = SIZE - 1;
  bool goingUp = true;
  while (col > 0) {
    if (col == 6) col -= 1;
    for (int8_t i = 0; i < SIZE; ++i) {
      const int8_t row = goingUp ? static_cast<int8_t>(SIZE - 1 - i) : i;
      for (int8_t cc = col; cc >= col - 1; --cc) {
        if (isFunctionModule(row, cc)) continue;
        const bool bit = (bitI < 208) ? allBits[bitI] : false;
        ++bitI;
        m[row][cc] = bit ^ ((row + cc) % 2 == 0);  // mask 0
      }
    }
    goingUp = !goingUp;
    col -= 2;
  }

  // Format info (co dinh EC=Q/mask=0) - 2 ban sao theo dung vi tri chuan QR.
  static const int8_t FA_R[15] = {8,8,8,8,8,8,8,8,7,5,4,3,2,1,0};
  static const int8_t FA_C[15] = {0,1,2,3,4,5,7,8,8,8,8,8,8,8,8};
  static const int8_t FB_R[15] = {20,19,18,17,16,15,14,8,8,8,8,8,8,8,8};
  static const int8_t FB_C[15] = {8,8,8,8,8,8,8,13,14,15,16,17,18,19,20};
  for (int i = 0; i < 15; ++i) {
    const bool bit = (FORMAT_BITS >> (14 - i)) & 1U;
    m[FA_R[i]][FA_C[i]] = bit;
    m[FB_R[i]][FB_C[i]] = bit;
  }
}

// Ma hoa "text" (PHAI dung 16 ky tu, dinh dang Device ID) vao "m". Tra ve
// false (khong dung "m") neu text sai dinh dang - nguoi goi phai tu kiem
// tra gia tri tra ve, TRANH ve QR rong/sai neu deviceId chua san sang.
bool encode(const char *text, bool m[SIZE][SIZE]) {
  uint8_t data[13], ec[13];
  if (!buildDataCodewords(text, data)) return false;
  rsEcCodewords(data, ec);
  buildMatrix(data, ec, m);
  return true;
}
}  // namespace QrMapId

// Coi active chi dieu khien ON/OFF qua transistor/MOSFET.
// "Nhe/to" duoc tao bang do dai va nhip keu, khong PWM relay.
struct BuzzerPattern {
  uint16_t onMs;
  uint16_t offMs;
  uint8_t pulses;
};

struct BuzzerState {
  uint32_t acknowledgedAlarmMask = 0;
  uint32_t acknowledgedAt[8] = {};
  uint32_t soundingAlarmBit = AlarmNone;
  uint32_t deadline = 0;
  bool outputOn = false;
  bool transientActive = false;
  bool transientOn = false;
  bool transientMandatory = false;
  bool resumePromptActive = false;
  bool turnPatternActive = false;
  uint8_t turnPhase = 0;
  uint8_t emergencyPhase = 0;
  uint8_t transientPulsesLeft = 0;
  BuzzerPattern transientPattern{0, 0, 0};
  // Giai dieu ngan phat 1 lan luc khoi dong (xem buzzerPlayStartupChime()).
  // Coi la loai active (chi ON/OFF, khong doi duoc cao do) nen "giai dieu"
  // o day la mot nhip do dai/khoang lang khac nhau, khong phai cac not nhac
  // that su - xem chu thich STARTUP_CHIME_MS.
  bool startupActive = false;
  uint8_t startupPhase = 0;
  // "Coi thong minh" cho AlarmTempHigh (xem buzzerUpdate()): thay vi keu lai
  // theo 1 hen gio co dinh (CRITICAL_RESOUND_MS) bat ke nhiet do dang lam gi,
  // theo doi xu huong nhiet tu luc ACK - con dang GIAM thi giu im (ma loi van
  // hien tren man hinh), nhiet dung yen hoac tang tro lai thi keu lai NGAY.
  float tempHighAckBestTemp = NAN;
  uint32_t tempHighAckLastSampleAt = 0;
};

BuzzerState buzzer;

void buzzerBegin();
void buzzerUpdate(uint32_t now);
void buzzerPlayCue(BuzzerCue cue);
void buzzerAcknowledge(uint32_t alarmMask);
void buzzerPlayStartupChime();
BuzzerPattern buzzerCuePattern(BuzzerCue cue);
BuzzerPattern alarmPattern(uint32_t bit);

static_assert(std::is_standard_layout<MachineConfig>::value, "MachineConfig phai la standard-layout");
static_assert(std::is_trivially_copyable<MachineConfig>::value,
              "MachineConfig phai copy duoc trong mailbox");
static_assert(std::is_trivially_copyable<MachineRuntime>::value,
              "MachineRuntime phai copy duoc trong mailbox");
static_assert(std::is_trivially_copyable<HmiEventSnapshot>::value,
              "HmiEventSnapshot phai copy duoc trong mailbox");

enum class ButtonEvent : uint8_t { None, ShortPress, LongPress };
struct RotaryState {
  uint8_t previousAB = 0;
  int16_t accumulator = 0;
  int8_t step = 0;
  bool lastRawButton = HIGH;
  bool stableButton = HIGH;
  uint32_t rawChangedAt = 0;
  uint32_t pressedAt = 0;
  bool longPressReported = false;
  ButtonEvent button = ButtonEvent::None;
};
RotaryState rotary;

// Dat bang giai ma trong DRAM de ISR rotary khong phu thuoc cache flash.
DRAM_ATTR static const int8_t QUADRATURE_TABLE[16] = {
   0, -1,  1,  0,  1,  0,  0, -1,
  -1,  0,  0,  1,  0,  1, -1,  0
};

uint8_t readAB() {
#if MAYAP_HMI_ENCODER_INTERRUPT
  return static_cast<uint8_t>(
      (gpio_get_level(static_cast<gpio_num_t>(PIN_ENCODER_CLK)) << 1) |
       gpio_get_level(static_cast<gpio_num_t>(PIN_ENCODER_DT)));
#else
  return static_cast<uint8_t>(
      (digitalRead(PIN_ENCODER_CLK) << 1) | digitalRead(PIN_ENCODER_DT));
#endif
}

#if MAYAP_HMI_ENCODER_INTERRUPT
portMUX_TYPE rotaryIsrMux = portMUX_INITIALIZER_UNLOCKED;
volatile int16_t rotaryPendingTransitions = 0;
volatile uint8_t rotaryIsrPreviousAB = 0;
bool rotaryInterruptReady = false;

void IRAM_ATTR rotaryEncoderIsr(void *) {
  const uint8_t ab = static_cast<uint8_t>(
      (gpio_ll_get_level(&GPIO, PIN_ENCODER_CLK) << 1) |
       gpio_ll_get_level(&GPIO, PIN_ENCODER_DT));

  portENTER_CRITICAL_ISR(&rotaryIsrMux);
  const uint8_t previous = rotaryIsrPreviousAB;
  rotaryIsrPreviousAB = ab;
  const int8_t delta = QUADRATURE_TABLE[(previous << 2) | ab];
  if (delta) {
    int16_t pending = static_cast<int16_t>(rotaryPendingTransitions + delta);
    // Gioi han backlog khi day encoder bi nhieu lien tuc.
    if (pending > 64) pending = 64;
    if (pending < -64) pending = -64;
    rotaryPendingTransitions = pending;
  }
  portEXIT_CRITICAL_ISR(&rotaryIsrMux);
}
#endif

void beginRotary() {
  pinMode(PIN_ENCODER_CLK, INPUT_PULLUP);
  pinMode(PIN_ENCODER_DT, INPUT_PULLUP);
  pinMode(PIN_ENCODER_SW, INPUT_PULLUP);
  rotary.previousAB = readAB();
#if MAYAP_HMI_ENCODER_INTERRUPT
  rotaryIsrPreviousAB = rotary.previousAB;
  if (mayapEnsureCacheSafeGpioService()) {
    const gpio_num_t clk = static_cast<gpio_num_t>(PIN_ENCODER_CLK);
    const gpio_num_t dt = static_cast<gpio_num_t>(PIN_ENCODER_DT);
    rotaryInterruptReady = gpio_set_intr_type(clk, GPIO_INTR_ANYEDGE) == ESP_OK &&
        gpio_set_intr_type(dt, GPIO_INTR_ANYEDGE) == ESP_OK &&
        gpio_isr_handler_add(clk, rotaryEncoderIsr, nullptr) == ESP_OK &&
        gpio_isr_handler_add(dt, rotaryEncoderIsr, nullptr) == ESP_OK;
    if (!rotaryInterruptReady) {
      (void)gpio_isr_handler_remove(clk);
      (void)gpio_isr_handler_remove(dt);
    }
  }
#endif
  rotary.lastRawButton = digitalRead(PIN_ENCODER_SW);
  rotary.stableButton = rotary.lastRawButton;
  rotary.rawChangedAt = millis();
  rotary.pressedAt = rotary.rawChangedAt;
}

void updateRotary(uint32_t now) {
  rotary.step = 0;
  rotary.button = ButtonEvent::None;

  int16_t transitionDelta = 0;
#if MAYAP_HMI_ENCODER_INTERRUPT
  if (rotaryInterruptReady) {
    portENTER_CRITICAL(&rotaryIsrMux);
    transitionDelta = rotaryPendingTransitions;
    rotaryPendingTransitions = 0;
    portEXIT_CRITICAL(&rotaryIsrMux);
  } else
#endif
  {
    const uint8_t ab = readAB();
    if (ab != rotary.previousAB) {
      transitionDelta = QUADRATURE_TABLE[(rotary.previousAB << 2) | ab];
      rotary.previousAB = ab;
    }
  }

  rotary.accumulator = static_cast<int16_t>(rotary.accumulator + transitionDelta);
  int16_t detents = rotary.accumulator / ENCODER_STEPS_PER_DETENT;
  if (detents) {
    int16_t emitted = detents;
    if (emitted > ENCODER_MAX_STEPS_PER_UPDATE) {
      emitted = ENCODER_MAX_STEPS_PER_UPDATE;
    } else if (emitted < -static_cast<int16_t>(ENCODER_MAX_STEPS_PER_UPDATE)) {
      emitted = -static_cast<int16_t>(ENCODER_MAX_STEPS_PER_UPDATE);
    }
    rotary.accumulator = static_cast<int16_t>(
        rotary.accumulator - emitted * ENCODER_STEPS_PER_DETENT);
    rotary.step = static_cast<int8_t>(REVERSE_ENCODER ? -emitted : emitted);
  }

  const bool raw = digitalRead(PIN_ENCODER_SW);
  if (raw != rotary.lastRawButton) {
    rotary.lastRawButton = raw;
    rotary.rawChangedAt = now;
  }
  const uint32_t debounceMs = raw == HIGH ? BUTTON_RELEASE_DEBOUNCE_MS : BUTTON_DEBOUNCE_MS;
  if (raw != rotary.stableButton && now - rotary.rawChangedAt >= debounceMs) {
    rotary.stableButton = raw;
    if (raw == LOW) {
      rotary.pressedAt = now;
      rotary.longPressReported = false;
    } else if (!rotary.longPressReported) {
      // Neu HMI bi tre dung luc nha nut, van phan loai theo thoi gian giu.
      rotary.button = now - rotary.pressedAt >= BUTTON_LONG_PRESS_MS ?
          ButtonEvent::LongPress : ButtonEvent::ShortPress;
    }
  }

  // Phat LongPress ngay khi du thoi gian, khong doi nguoi dung nha nut.
  if (raw == LOW && rotary.stableButton == LOW && !rotary.longPressReported &&
      now - rotary.pressedAt >= BUTTON_LONG_PRESS_MS) {
    rotary.longPressReported = true;
    rotary.button = ButtonEvent::LongPress;
  }
  if (rotary.button != ButtonEvent::None) {
    mayapSerialPrintf(false, "[HMI-KEY] %s hold=%lums\n",
        rotary.button == ButtonEvent::LongPress ? "LONG" : "SHORT",
        static_cast<unsigned long>(now - rotary.pressedAt));
  }
}

// ============================================================
// 4. BANG CAI DAT - THEM THONG SO KHONG CAN THEM MAN HINH MOI
// ============================================================
enum class SettingType : uint8_t { Bool, U8, U16, Float };

struct SettingItem {
  const char *label;
  SettingType type;
  uint16_t offset;
  float minimum;
  float maximum;
  float step;
  uint8_t decimals;
  const char *unit;
  const char *const *options;
  uint8_t optionCount;
};

// Khai bao prototype thu cong de tranh loi Arduino .ino auto-prototype
// voi kieu SettingItem duoc khai bao sau cac ham dau tien trong sketch.
float readSetting(const MachineConfig &cfg, const SettingItem &item);
void writeSetting(MachineConfig &cfg, const SettingItem &item, float value);
void normalizeConfig(MachineConfig &cfg);
void sanitizeConfig(MachineConfig &cfg);
void settingLimits(const MachineConfig &cfg, const SettingItem &item, float &minimum, float &maximum);
void formatSettingValue(const SettingItem &item, float value, char *out, size_t size);

const char *const OPT_OFF_ON[] = {"TAT", "BAT"};
const char *const OPT_NO_YES[] = {"KHONG", "CO"};
const char *const OPT_OFFLINE_ONLINE[] = {"OFFLINE", "ONLINE"};
const char *const OPT_ON_OFF_HOI[] = {"HOI XN", "TU DONG"};
const char *const OPT_VENT_LEVEL[] = {"THAP", "TIEU CHUAN", "CAO"};
#define ITEM_FLOAT(lbl, member, mn, mx, st, dec, unitText) \
  {lbl, SettingType::Float, offsetof(MachineConfig, member), mn, mx, st, dec, unitText, nullptr, 0}
#define ITEM_U8(lbl, member, mn, mx, st, unitText) \
  {lbl, SettingType::U8, offsetof(MachineConfig, member), mn, mx, st, 0, unitText, nullptr, 0}
#define ITEM_U8_OPTIONS(lbl, member, opts, count) \
  {lbl, SettingType::U8, offsetof(MachineConfig, member), 0, (count) - 1, 1, 0, "", opts, count}
#define ITEM_U16(lbl, member, mn, mx, st, unitText) \
  {lbl, SettingType::U16, offsetof(MachineConfig, member), mn, mx, st, 0, unitText, nullptr, 0}
#define ITEM_BOOL_OPTIONS(lbl, member, opts) \
  {lbl, SettingType::Bool, offsetof(MachineConfig, member), 0, 1, 1, 0, "", opts, 2}
#define ITEM_BOOL(lbl, member) ITEM_BOOL_OPTIONS(lbl, member, OPT_OFF_ON)
// v3.5.0: "CAI DAT ME" chi giu 4 thong so thao tac hang ngay; moi thu con lai
// don sang cac thu muc trong "CAI DAT CHUNG" (xem GROUPS/ChungMenu ben duoi).
const SettingItem SETTINGS[] = {
  // ---- CAI DAT ME (4 muc) ----
  ITEM_FLOAT("Nhiet do ap", targetTemp, TARGET_TEMP_MIN_C,
             TARGET_TEMP_MAX_C, 0.1f, 1, "C"),                                  // 0
  ITEM_FLOAT("Canh bao am", lowHumidityAlarm, 10.0f, 90.0f, 1.0f, 0, "%"),      // 1 (bao het nuoc)
  ITEM_BOOL_OPTIONS("Ap lai", autoResumeOnPowerLoss, OPT_ON_OFF_HOI),           // 2
  ITEM_U8("So ngay ap", totalIncubationDays, 1, 40, 1, "ng"),                   // 3

  // ---- CAI DAT CHUNG > NHIET DO (5 muc) ----
  // Chuoi nhiet tren LCD duoc rang buoc dong bo:
  // Bao thap < SV <= Hut tat < Hut bat <= Bao cao < Bao khan cap.
  ITEM_FLOAT("Bao thap", lowTempAlarm, 25.0f,
             TARGET_TEMP_MAX_C - LOW_ALARM_GAP_C, 0.1f, 1, "C"),               // 4
  ITEM_FLOAT("Bao cao", highTempAlarm,
             TARGET_TEMP_MIN_C + HIGH_ALARM_GAP_C,
             HIGH_ALARM_MAX_C, 0.1f, 1, "C"),                                  // 5
  ITEM_FLOAT("Bao khan cap", emergencyTemp,
             TARGET_TEMP_MIN_C + HIGH_ALARM_GAP_C + EMERGENCY_ABOVE_HIGH_C,
             EMERGENCY_MAX_C, 0.1f, 1, "C"),                                   // 6
  // Bu sai so cam bien (config_.tempOffset) - da co san tren web (form Cam
  // bien) va da luu EEPROM tu truoc, chi moi them vao HMI de chinh truc tiep
  // tren may khong can mo web. Dung chung field/gioi han voi machine_control.h
  // sanitizeConfig() (clampFloat -5..5).
  ITEM_FLOAT("Bu nhiet do", tempOffset, -5.0f, 5.0f, 0.1f, 1, "C"),            // 7
  // BAT (mac dinh): Bao cao/Bao khan cap hoat dong moi luc, ke ca khong co
  // me ap. TAT: 2 canh bao nay CHI kiem tra khi dang co me (giong Bao thap).
  // Ten rut gon tu "Bao nhiet ngoai me" - o dong nay gia tri la 1 trong 2 tuy
  // chon NGAN ("TAT"/"BAT", 3 ky tu), nen kich thuoc danh cho nhan gan nhu
  // toi da (xem drawSettingList()); "Bao nhiet ngoai me" (18 ky tu) dai hon
  // muc cho phep nen luon bi ep xuong font nho hon cac dong khac trong danh
  // sach - nhin giong loi font dai loi day du nay chi con 15 ky tu de luon
  // ve duoc bang font lon, dong bo voi cac dong con lai.
  ITEM_BOOL("Bao nhiet ngoai", highTempAlarmWithoutBatch),                     // 8

  // ---- CAI DAT CHUNG > QUAT HUT (2 muc) ----
  ITEM_FLOAT("Quat hut bat", ventOnTemp, TARGET_TEMP_MIN_C + VENT_ON_ABOVE_SV_C,
             HIGH_ALARM_MAX_C, 0.1f, 1, "C"),                                  // 9
  ITEM_FLOAT("Quat hut tat", ventOffTemp, TARGET_TEMP_MIN_C,
             HIGH_ALARM_MAX_C, 0.1f, 1, "C"),                                  // 10

  // ---- CAI DAT CHUNG > DAO TRUNG (3 muc, + "So lan dao" la dong phu) ----
  ITEM_BOOL("Tu dong dao", turningEnabled),                                    // 11
  ITEM_U16("Chu ky dao", turnIntervalMin, 1, 720, 1, "ph"),                   // 12
  ITEM_U16("Tre loi dao", turnMaxRunSec, 5, 600, 5, "s"),                    // 13

  // ---- CAI DAT CHUNG > KET NOI (1 muc, + "Doi wifi" la dong phu) ----
  // Ten rut gon tu "Che do ket noi" (14 ky tu) - option dai nhat la "OFFLINE"
  // (7 ky tu) chiem het phan lon 128px con lai, nen nhan 14 ky tu luon bi ep
  // ve font nho (xem drawSettingList()). Da o ngay man hinh header "HE
  // THONG" nen rut gon con "Ket noi" (7 ky tu) van ro nghia va luon ve duoc
  // bang font lon nhu cac dong khac.
  ITEM_U8_OPTIONS("Ket noi", connectivityMode,
                  OPT_OFFLINE_ONLINE, 2),                                     // 14

  // ---- NANG CAO (12 muc hien + 1 slot legacy an): PID thu cong + nguong chan doan nhiet + Auto
  // Tune - them de nguoi dung tu chinh khi lap may that (dan nhiet cong suat
  // lon co the can nguong khac mac dinh) ma khong can nap lai firmware. Gioi
  // han min/max khop dung sanitizeMachineConfig() trong machine_control.h.
  ITEM_FLOAT("He so Kp", kp, 0.0f, 100.0f, 0.5f, 1, ""),                     // 15
  ITEM_FLOAT("He so Ki", ki, 0.0f, 20.0f, 0.05f, 2, ""),                     // 16
  ITEM_FLOAT("He so Kd", kd, 0.0f, 200.0f, 1.0f, 1, ""),                     // 17
  // Hidden legacy slot 18; preserve fixed indexes used by ventilation menus.
  ITEM_U16("", pidCycleSec, 1, 60, 1, "s"),
  ITEM_U8("Tran cong suat", maxHeaterPower, 10, 100, 5, "%"),                // 19
  ITEM_FLOAT("Muc tang toi thieu", heaterStuckMinRiseC, 0.05f, 5.0f, 0.05f, 2, "C"), // 20
  ITEM_U16("TG khong tang nhiet", heaterStuckDurationSec, 60, 3600, 30, "s"), // 21
  ITEM_FLOAT("Nguong toc do", tempRateLimitC, 0.1f, 10.0f, 0.1f, 1, "C"),    // 22
  ITEM_U16("Khung toc do", tempRateWindowSec, 30, 1800, 10, "s"),            // 23
  ITEM_U8("So lan doi dau", tempOscillationCrossLimit, 2, 30, 1, "l"),       // 24
  ITEM_U16("Khung dao dong", tempOscillationWindowSec, 60, 3600, 30, "s"),   // 25
  ITEM_U8("Cong suat tune", autotuneRelayPowerPercent, 10, 80, 5, "%"),      // 26
  ITEM_FLOAT("Bien do tune", autotuneBandC, 0.05f, 1.0f, 0.05f, 2, "C"),     // 27
  ITEM_BOOL("Dao tay dong lich", manualTurnReanchorsSchedule),              // 28
  // Backlog (khong thuoc audit v3.7.1): tu kiem tra coi dinh ky, opt-in,
  // mac dinh TAT - xem sirenSelfTestEnabled trong config.h.
  ITEM_BOOL("Tu kiem tra coi", sirenSelfTestEnabled),                       // 29

  // ---- TAO AM (chi hien khi HE THONG khai bao co phan cung) ----
  ITEM_BOOL("Tao am", humidifierEnabled),                                      // 30
  ITEM_FLOAT("Do am dat", targetHumidity, 30.0f, 90.0f, 1.0f, 0, "%"),         // 31
  // ---- PHAN CUNG / THONG GIO DINH KY: them CUOI de giu index cu ----
  ITEM_BOOL_OPTIONS("Co bo tao am", humidifierInstalled, OPT_NO_YES),           // 32
  ITEM_BOOL("Thong gio dinh ky", ventScheduleEnabled),                         // 33
  ITEM_U8("So lan thong gio", ventScheduleCount, 1, VENT_SCHEDULE_MAX_RUNS, 1, "l"), // 34
  ITEM_U8("TG moi lan", ventScheduleDurationMin, 1, 60, 1, "ph"),             // 35
  ITEM_U8("Gio lan 1", ventScheduleHour1, 0, 23, 1, "h"),                     // 36
  ITEM_U8("Gio lan 2", ventScheduleHour2, 0, 23, 1, "h"),                     // 37
  ITEM_U8("Gio lan 3", ventScheduleHour3, 0, 23, 1, "h"),                     // 38
  ITEM_U8("Gio lan 4", ventScheduleHour4, 0, 23, 1, "h"),                     // 39
  ITEM_U8("Gio lan 5", ventScheduleHour5, 0, 23, 1, "h"),                     // 40
  ITEM_U8("Gio lan 6", ventScheduleHour6, 0, 23, 1, "h"),                     // 41
  ITEM_BOOL("Thong gio tu dong", ventAutoEnabled),                            // 42
  ITEM_U8_OPTIONS("Muc thong gio", ventProfileLevel, OPT_VENT_LEVEL, 3),      // 43
  ITEM_U8("Chu ky thong gio", ventCycleMinutes, 40, 120, 10, "ph"),           // 44
  ITEM_U8("Ngay 1-3", ventDutyDay1To3, 5, 90, 1, "%"),                       // 45
  ITEM_U8("Ngay 4-7", ventDutyDay4To7, 5, 90, 1, "%"),                       // 46
  ITEM_U8("Ngay 8-11", ventDutyDay8To11, 5, 90, 1, "%"),                     // 47
  ITEM_U8("Ngay 12-15", ventDutyDay12To15, 5, 90, 1, "%"),                   // 48
  ITEM_U8("Ngay 16-18", ventDutyDay16To18, 5, 90, 1, "%"),                   // 49
  ITEM_U8("Ngay 19-21", ventDutyDay19To21, 5, 90, 1, "%"),                    // 50
  ITEM_BOOL("Tu can bang nhiet", adaptiveThermalBalanceEnabled)               // 51
};

constexpr uint8_t SETTING_COUNT = sizeof(SETTINGS) / sizeof(SETTINGS[0]);
static_assert(SETTING_COUNT == 52, "Bang SETTINGS phai co 52 thong so");

const uint8_t GROUP_SETTING_INDEXES[] = {
  0,1,2,3,                             // Cai dat me
  4,5,6,7,8,                           // Nhiet do
  11,12,13,28,                         // Dao trung
  14,29,32,                            // He thong
  15,16,17,19,51,                      // PID / SSR
  20,21,22,23,24,25,26,27,             // Bao ve nhiet
  9,10,                                // Quat hut; Thong gio la menu con
  30,31                                // Tao am
};

struct SettingGroup { const char *label; uint8_t first; uint8_t count; };
// Chi so 0 = Cai dat me (goc tu MainMenu); con lai la thu muc con cua
// "CAI DAT CHUNG" (goc tu ChungMenu). Dung chung mot co che SettingList.
// Nhom TAO AM phai o cuoi; groupExtraSlot() tham chieu co dinh nhom 1/2/3.
const SettingGroup GROUPS[] = {
  {"CAI DAT ME", 0, 4},
  // Nguong quat hut da duoc tach sang nhom QUAT HUT.
  {"NHIET DO", 4, 5},
  {"DAO TRUNG", 9, 4},
  // Doi ten tu "KET NOI" thanh "HE THONG": nhom nay tu lau da khong chi con
  // la cai dat mang - gom ca ma QR, dat lai PIN, cap nhat firmware... nen
  // "He thong" mo ta dung hon la cai dat chung cua may.
  {"HE THONG", 13, 3},
  {"PID / SSR", 16, 5},
  {"BAO VE NHIET", 21, 8},
  {"QUAT HUT", 29, 2},
  {"TAO AM", 31, 2}
};
constexpr uint8_t GROUP_COUNT = sizeof(GROUPS) / sizeof(GROUPS[0]);
static_assert(GROUP_COUNT == 8, "Bang GROUPS phai co 8 nhom");
static_assert(sizeof(GROUP_SETTING_INDEXES) / sizeof(GROUP_SETTING_INDEXES[0]) == 33U,
              "Sai so luong setting hien trong menu chinh");

// Dong phu (khong phai setting gia tri) duoc gan them vao cuoi mot so nhom.
// Moi nhom co toi da 5 dong phu (hien tai chi HE THONG dung ca 5) - liet ke
// theo THU TU CO DINH qua groupExtraSlot(), roi loc bot dong dang AN (vd
// "Doi wifi" chi hien khi Online) qua visibleGroupExtraAt() de ra danh sach
// LIEN TUC hien thi tren man hinh. (groupExtraSlotVisible/visibleGroupExtraAt/
// groupVisibleExtraCount dat o DUOI, sau khai bao currentConfig - xem do.)
enum class GroupExtra : uint8_t { None, TurnStats, WifiChange, ConnectionInfo, QrCode, CloudPinReset, FirmwareWebUpdate, AutoTuneEntry, VentilationEntry };
GroupExtra groupExtraSlot(uint8_t group, uint8_t slot) {
  // NHIET DO -> "Tu chinh PID" (chuyen tu 1 muc rieng trong Cai dat chung
  // vao day - Auto Tune tu do thong so nhiet nen hop ly hon khi gan voi
  // nhom Nhiet do, giong cach "So lan dao" gan voi Dao trung).
  if (group == 1 && slot == 0) return GroupExtra::AutoTuneEntry;
  if (group == 2 && slot == 0) return GroupExtra::TurnStats;      // DAO TRUNG -> "So lan dao"
  // Thu tu da quy hoach lai (nhom HE THONG): 2 muc ve MANG di lien nhau
  // truoc (thong tin + doi wifi), roi den DINH DANH/BAO MAT (QR + PIN), cuoi
  // cung la CAP NHAT (dung chung root voi may, it thao tac nhat).
  if (group == 3 && slot == 0) return GroupExtra::ConnectionInfo;   // HE THONG -> "Thong tin ket noi"
  if (group == 3 && slot == 1) return GroupExtra::WifiChange;       // HE THONG -> "Doi wifi"
  if (group == 3 && slot == 2) return GroupExtra::QrCode;           // HE THONG -> "Ma QR ID"
  if (group == 3 && slot == 3) return GroupExtra::CloudPinReset;    // HE THONG -> "Dat lai ma PIN"
  if (group == 3 && slot == 4) return GroupExtra::FirmwareWebUpdate;
  // Menu Thong gio la nhanh con cua QUAT HUT.
  if (group == 6 && slot == 0) return GroupExtra::VentilationEntry;
  return GroupExtra::None;
}

float readSetting(const MachineConfig &cfg, const SettingItem &item) {
  const uint8_t *base = reinterpret_cast<const uint8_t *>(&cfg) + item.offset;
  switch (item.type) {
    case SettingType::Bool: return *reinterpret_cast<const bool *>(base) ? 1.0f : 0.0f;
    case SettingType::U8: return *reinterpret_cast<const uint8_t *>(base);
    case SettingType::U16: return *reinterpret_cast<const uint16_t *>(base);
    case SettingType::Float: return *reinterpret_cast<const float *>(base);
  }
  return 0;
}

void writeSetting(MachineConfig &cfg, const SettingItem &item, float value) {
  value = constrain(value, item.minimum, item.maximum);
  uint8_t *base = reinterpret_cast<uint8_t *>(&cfg) + item.offset;
  switch (item.type) {
    case SettingType::Bool: *reinterpret_cast<bool *>(base) = value >= 0.5f; break;
    case SettingType::U8: *reinterpret_cast<uint8_t *>(base) = static_cast<uint8_t>(lroundf(value)); break;
    case SettingType::U16: *reinterpret_cast<uint16_t *>(base) = static_cast<uint16_t>(lroundf(value)); break;
    case SettingType::Float: *reinterpret_cast<float *>(base) = value; break;
  }
}

void normalizeConfig(MachineConfig &cfg) {
  // Chuoi rang buoc truc tiep theo SV, moi muc cach nhau toi thieu 0.1 C.
  cfg.lowTempAlarm = fminf(cfg.lowTempAlarm,
                           cfg.targetTemp - LOW_ALARM_GAP_C);

  cfg.highTempAlarm = fmaxf(cfg.highTempAlarm,
                            cfg.targetTemp + HIGH_ALARM_GAP_C);
  cfg.highTempAlarm = fminf(cfg.highTempAlarm, HIGH_ALARM_MAX_C);

  cfg.emergencyTemp = fmaxf(cfg.emergencyTemp,
                            cfg.highTempAlarm + EMERGENCY_ABOVE_HIGH_C);
  cfg.emergencyTemp = fminf(cfg.emergencyTemp, EMERGENCY_MAX_C);

  // Rang buoc dong bo quat hut voi SV va Bao cao:
  // SV <= Hut tat < Hut bat <= Bao cao.
  const float ventOnMin = cfg.targetTemp + VENT_ON_ABOVE_SV_C;
  const float ventOnMax = cfg.highTempAlarm;
  cfg.ventOnTemp = constrain(cfg.ventOnTemp, ventOnMin, ventOnMax);

  const float ventOffMin = cfg.targetTemp + VENT_OFF_ABOVE_SV_C;
  const float ventOffMax = cfg.ventOnTemp - VENT_HYSTERESIS_C;
  cfg.ventOffTemp = constrain(cfg.ventOffTemp, ventOffMin, ventOffMax);
}

// Chi rang buoc theo SV (targetTemp) - gia tri LUON moi nhat - thay vi rang
// buoc CHEO giua cac nguong voi nhau (vd Bao cao truoc day bi khoa toi thieu
// bang Hut bat). Kieu rang buoc cheo cu co 1 loi thuc te: neu nguoi dung doi
// SV qua trang wed bang form "nhanh" (chi doi rieng SV, khong keo theo cac
// nguong con lai), cac nguong con lai tro thanh "cu" - va sau do tren HMI,
// nguoi dung KHONG THE chinh 1 nguong nao do ve gan SV moi duoc nua vi bi
// chinh cai nguong lac hau do khoa lai (vd: SV=30 nhung Bao cao chi cho
// chinh xuong toi 38.1, dung ra phai xuong duoc 30.1). Thu tu tuong doi
// giua cac nguong (Bao thap<SV<=Hut tat<Hut bat<=Bao cao<Khan cap) van duoc
// dam bao dung tai thoi diem LUU thuc su qua sanitizeMachineConfig() trong
// machine_control.h - ham do luon tinh lai toan bo chuoi tu SV moi nhat va
// tu keo cac nguong con lai ve dung vi tri, khong can khoa nguoi dung lai o
// day. Xem thêm buildConfig() trong app.js - form "nhanh" tren wed cung da
// duoc sua de tu keo theo cac nguong nay khi doi SV, tranh loi lac hau xay
// ra tu dau.
void settingLimits(const MachineConfig &cfg, const SettingItem &item,
                   float &minimum, float &maximum) {
  minimum = item.minimum; maximum = item.maximum;
  const uint16_t offset = item.offset;
  if (offset == offsetof(MachineConfig, lowTempAlarm)) {
    maximum = fminf(maximum, cfg.targetTemp - LOW_ALARM_GAP_C);
  } else if (offset == offsetof(MachineConfig, highTempAlarm)) {
    minimum = fmaxf(minimum, cfg.targetTemp + HIGH_ALARM_GAP_C);
  } else if (offset == offsetof(MachineConfig, emergencyTemp)) {
    minimum = fmaxf(minimum,
        cfg.targetTemp + HIGH_ALARM_GAP_C + EMERGENCY_ABOVE_HIGH_C);
  } else if (offset == offsetof(MachineConfig, ventOnTemp)) {
    minimum = fmaxf(minimum, cfg.targetTemp + VENT_ON_ABOVE_SV_C);
  } else if (offset == offsetof(MachineConfig, ventOffTemp)) {
    minimum = fmaxf(minimum, cfg.targetTemp + VENT_OFF_ABOVE_SV_C);
  }
  if (minimum > maximum) minimum = maximum;
}

void sanitizeConfig(MachineConfig &cfg) {
  // Du lieu hong/NaN/Inf khong duoc phep di vao giao dien hoac PID.
  const MachineConfig defaults{};
  for (uint8_t i = 0; i < SETTING_COUNT; ++i) {
    float value = readSetting(cfg, SETTINGS[i]);
    if (!isfinite(value)) value = readSetting(defaults, SETTINGS[i]);
    writeSetting(cfg, SETTINGS[i], value);
  }
  if (!cfg.humidifierInstalled) cfg.humidifierEnabled = false;

  if (!isfinite(cfg.tempHysteresis)) cfg.tempHysteresis = defaults.tempHysteresis;
  cfg.tempHysteresis = constrain(cfg.tempHysteresis, 0.1f, 1.0f);

  // PID luon la che do dieu khien cua san pham. Nguoi dung chi bam Auto Tune.
  cfg.controlMode = ControlMode::Pid;
  if (!isfinite(cfg.kp)) cfg.kp = defaults.kp;
  if (!isfinite(cfg.ki)) cfg.ki = defaults.ki;
  if (!isfinite(cfg.kd)) cfg.kd = defaults.kd;
  cfg.kp = constrain(cfg.kp, 0.0f, 100.0f);
  cfg.ki = constrain(cfg.ki, 0.0f, 20.0f);
  cfg.kd = constrain(cfg.kd, 0.0f, 200.0f);
  cfg.pidCycleSec = static_cast<uint16_t>(constrain(
      static_cast<int>(cfg.pidCycleSec), 1, 60));
  cfg.maxHeaterPower = static_cast<uint8_t>(constrain(
      static_cast<int>(cfg.maxHeaterPower), 10, 100));

  if (!isfinite(cfg.emergencyTemp)) cfg.emergencyTemp = defaults.emergencyTemp;
  if (!isfinite(cfg.ventOnTemp)) cfg.ventOnTemp = defaults.ventOnTemp;
  if (!isfinite(cfg.ventOffTemp)) cfg.ventOffTemp = defaults.ventOffTemp;
  cfg.emergencyTemp = constrain(
      cfg.emergencyTemp,
      TARGET_TEMP_MIN_C + HIGH_ALARM_GAP_C + EMERGENCY_ABOVE_HIGH_C,
      EMERGENCY_MAX_C);
  cfg.ventOnTemp = constrain(cfg.ventOnTemp, 30.0f, 42.0f);
  cfg.ventOffTemp = constrain(cfg.ventOffTemp, 25.0f, 41.0f);

  cfg.humidityAlarmDelaySec = static_cast<uint16_t>(constrain(
      static_cast<int>(cfg.humidityAlarmDelaySec), 0, 600));
  cfg.powerRestoreDelaySec = static_cast<uint16_t>(constrain(
      static_cast<int>(cfg.powerRestoreDelaySec), 0, 600));
  if (!isfinite(cfg.tempOffset)) cfg.tempOffset = defaults.tempOffset;
  if (!isfinite(cfg.humidityOffset)) cfg.humidityOffset = defaults.humidityOffset;
  cfg.tempOffset = constrain(cfg.tempOffset, -5.0f, 5.0f);
  cfg.humidityOffset = constrain(cfg.humidityOffset, -20.0f, 20.0f);
  // Legacy slot only; runtime sensor freshness is fixed fail-safe policy.
  cfg.sensorTimeoutSec = defaults.sensorTimeoutSec;
  if (static_cast<uint8_t>(cfg.nextDirection) >
      static_cast<uint8_t>(TurnDirection::Right)) {
    cfg.nextDirection = defaults.nextDirection;
  }
  if (static_cast<uint8_t>(cfg.connectivityMode) >
      static_cast<uint8_t>(ConnectivityMode::Online)) {
    cfg.connectivityMode = defaults.connectivityMode;
  }
  cfg.alarmEnabled = true;
  normalizeConfig(cfg);
}

void formatSettingValue(const SettingItem &item, float value, char *out, size_t size) {
  if (item.options && item.optionCount > 0) {
    const uint8_t index = static_cast<uint8_t>(constrain(lroundf(value), 0L,
                                  static_cast<long>(item.optionCount - 1U)));
    snprintf(out, size, "%s", item.options[index]);
  } else if (item.decimals == 0) {
    snprintf(out, size, "%ld%s", lroundf(value), item.unit);
  } else {
    snprintf(out, size, "%.*f%s", item.decimals, value, item.unit);
  }
}

// ============================================================
// 5. TRANG, MENU, HANG DOI LENH
// ============================================================
enum class View : uint8_t {
  Home, MainMenu, ChungMenu, SettingList, EditSetting, TurnStats, AutoTune,
  EventLog, Alarm, TestMode, TestSummary, WifiChange, ConnectionInfo, QrCode,
  FirmwareProgress, TurnStatus, VentilationMenu, VentilationAdvanced, FirmwareMenu
};

enum class ConfirmAction : uint8_t { None, BatchToggle, AutoTuneStart, ResumeBatch, TurningToggle, CloudPinReset, FirmwareWebApply, FirmwareRollback, BatchOverdueContinue, AutoTuneCancel };

// Prototype thu cong: Arduino IDE tu sinh prototype cho ham trong .ino.
// Neu ham dung enum/struct tuy chinh, prototype tu dong co the bi chen
// truoc noi khai bao kieu va gay loi "View was not declared".
void openBatchConfirm(View returnView);
void openResumeConfirm();
void openBatchOverdueContinueConfirm();
void openAlarmView(View returnView);
void openTestMode();

MachineConfig currentConfig;
MachineRuntime currentRuntime;
HmiEventSnapshot currentEventLog;

// Tiep tuc dinh nghia GroupExtra (xem groupExtraSlot() o tren) - 3 ham nay
// doc currentConfig nen phai dat SAU khai bao no.
bool groupExtraSlotVisible(GroupExtra extra) {
  // Ca 2 hanh dong can Cloud (Doi wifi/Dat lai PIN) deu chi hien khi dang
  // Online - offline thi bam vao cung khong lam duoc gi (khong ket noi toi
  // Worker), an di do tranh nguoi dung bam nham roi thac mac sao khong chay.
  if (extra == GroupExtra::WifiChange || extra == GroupExtra::CloudPinReset) {
    return currentConfig.connectivityMode == ConnectivityMode::Online;
  }
  // LUON hien khi Online (giong Doi wifi/Dat lai PIN) - bam vao se hien
  // phien ban hien tai neu chua co ban moi, hoac man hinh xac nhan Co/Khong
  // neu THAT SU co ban moi dang cho (xem openFirmwareWebConfirm()). Truoc
  // day chi hien khi co ban moi nen nguoi dung khong co cach nao vao xem
  // "may dang chay ban may" tu HMI.
  if (extra == GroupExtra::FirmwareWebUpdate) return true;
  // Chi hien khi THAT SU con 1 ban firmware truoc do hop le trong vi tri
  // OTA con lai (xem mayapOtaRollbackBegin() trong ota_rollback.h, kiem tra
  // 1 lan luc khoi dong) - may chua tung OTA lan nao thi khong co gi de
  // quay lai, an muc nay di thay vi de nguoi dung bam roi nhan loi.
  return extra != GroupExtra::None;
}
GroupExtra visibleGroupExtraAt(uint8_t group, uint8_t visibleIdx) {
  uint8_t seen = 0;
  // 6 slot (0..5): tang gioi han neu groupExtraSlot() co them muc.
  for (uint8_t slot = 0; slot < 6U; ++slot) {
    const GroupExtra extra = groupExtraSlot(group, slot);
    if (!groupExtraSlotVisible(extra)) continue;
    if (seen == visibleIdx) return extra;
    ++seen;
  }
  return GroupExtra::None;
}
uint8_t groupVisibleExtraCount(uint8_t group) {
  uint8_t count = 0;
  while (visibleGroupExtraAt(group, count) != GroupExtra::None) ++count;
  return count;
}
// Chieu nguoc lai cua visibleGroupExtraAt() - dung khi goBack() tu 1 man
// hinh con (WifiChange/ConnectionInfo/QrCode...) can biet dung VI TRI HIEN
// TAI cua dong phu tuong ung trong danh sach de chon lai dung dong (khong
// con hardcode vi tri co dinh - de sai neu sau nay doi thu tu groupExtraSlot()).
uint8_t visibleExtraIndexOf(uint8_t group, GroupExtra target) {
  uint8_t idx = 0;
  while (true) {
    const GroupExtra extra = visibleGroupExtraAt(group, idx);
    if (extra == target || extra == GroupExtra::None) return idx;
    ++idx;
  }
}

bool lcdReady = false;
bool dirty = true;

struct ConfigSaveTransaction {
  bool active = false;
  bool readyForHost = false;
  uint32_t id = 0;
  uint32_t startedAt = 0;
  MachineConfig rollback;
  MachineConfig candidate;
};
ConfigSaveTransaction configSave;
uint32_t nextConfigTransactionId = 1;

View view = View::Home;
uint8_t homePage = 0;
uint8_t mainIndex = 0;
uint8_t chungIndex = 0;
uint8_t chungTop = 0;
uint8_t listIndex = 0;
uint8_t listTop = 0;
uint8_t selectedGroup = 0;
uint32_t wifiPortalLastCommandAt = 0;
uint32_t testModeLastCommandAt = 0;
TestLimitId testLimitSelected = TestLimitId::Left;

constexpr uint8_t TEST_MODE_OUTPUT_ROWS = static_cast<uint8_t>(TestOutputId::Count);
constexpr uint8_t TEST_MODE_LIMIT_ROWS = 2U;
constexpr uint8_t TEST_MODE_ITEM_COUNT = static_cast<uint8_t>(
    TEST_MODE_OUTPUT_ROWS + TEST_MODE_LIMIT_ROWS + 1U);  // + Ket thuc
constexpr uint8_t TEST_SUMMARY_ITEM_COUNT = static_cast<uint8_t>(
    TEST_MODE_OUTPUT_ROWS + TEST_MODE_LIMIT_ROWS);

// Ket qua nguoi dung tu xac nhan cho tung thiet bi/cong tac hanh trinh, dung
// de tong ket lai trong View::TestSummary. Rieng cua HMI, khong can dong bo
// nguoc ve firmware tong vi day la tu danh gia bang mat/tai cua nguoi lap dat.
enum class TestResult : uint8_t { Untested, Pass, Fail };
TestResult testDeviceResult[TEST_MODE_OUTPUT_ROWS] = {};
TestResult testLimitResult[TEST_MODE_LIMIT_ROWS] = {};
TestLimitPhase lastObservedTestLimitPhase = TestLimitPhase::Idle;
// Workflow rieng cho SSR nhiet: 5s quat prestart -> toi da 3p gia nhiet ->
// hoi NHIET DA LEN? -> bat quat hut -> hoi QUAT HUT DA CHAY?.
enum class HeaterTestUiPhase : uint8_t { Idle, Heating, ConfirmHeat, ConfirmVent };
HeaterTestUiPhase heaterTestUiPhase = HeaterTestUiPhase::Idle;
uint32_t heaterTestUiStartedAt = 0U;
uint32_t heaterTestUiLastRefreshAt = 0U;
bool heaterTestConfirmYes = true;
bool testDeviceConfirmActive = false;
uint8_t testDeviceConfirmIndex = 0;
bool testDeviceConfirmYes = true;
uint8_t testSummaryIndex = 0;
float editValue = 0;
uint8_t editSettingIndex = 0;
ConfirmAction confirmAction = ConfirmAction::None;
View confirmReturnView = View::Home;
bool confirmYes = true;
// Cau hinh dang cho xac nhan CO/HUY cho ConfirmAction::TurningToggle (bat/tat
// dao tu dong) - chi ap dung khi thao tac nay, khong dung cho cac truong khac.
MachineConfig pendingTurningConfig;
bool resumeDecisionSubmitted = false;
// F-06: giong het resumeDecisionSubmitted nhung cho man hinh xac nhan "me
// qua han - tiep tuc u am?" (xem executeConfirmation()/updateBatchOverdue()).
bool batchOverdueDecisionSubmitted = false;
// Nhac dinh ky "co ban firmware moi" tren man hinh chinh - 0 nghia la "chua
// tung nhac, hien ngay lan dau tien co ban moi". Bam "Khong" se cap nhat gia
// tri nay thanh millis() hien tai de 12h sau moi nhac lai; bam "Co" thi
// khong can vi may se khoi dong lai.
uint32_t firmwareReminderLastAt = 0U;
constexpr uint32_t FIRMWARE_REMINDER_INTERVAL_MS = 12UL * 60UL * 60UL * 1000UL;
View alarmReturnView = View::Home;
uint8_t alarmIndex = 0;
uint8_t eventLogIndex = 0;
View editReturnView = View::SettingList;
uint8_t ventilationIndex = 0U;
uint8_t ventilationAdvancedIndex = 0U;
uint8_t firmwareMenuIndex = 0U;
View eventLogReturnView = View::MainMenu;
bool eventLogFaultsOnly = false;
constexpr uint8_t HMI_RECENT_FAULT_CAPACITY = 8U;
HmiEventItem recentFaults[HMI_RECENT_FAULT_CAPACITY]{};
uint32_t recentFaultCapturedAt[HMI_RECENT_FAULT_CAPACITY]{};
uint8_t recentFaultCount = 0U;
uint8_t lastEventWindowCount = 0U;
uint32_t lastEventSourceSequence = 0U;
uint32_t alarmPresentedMask = 0;
// Man phu tu dong bat khi dong co BAT DAU chay dao/tim goc (canh trong
// tick khi runtime.turnState chuyen sang Left/Right) - co che giong het
// man Alarm (xem applyRuntime()): tu bat khi co "su kien moi", tu tat khi
// dong co ngung, nguoi dung co the bam ngan de thoat som ve man truoc do.
View turnStatusReturnView = View::Home;

uint32_t lastDrawAt = 0;
uint32_t lastHomeDrawAt = 0;
uint32_t lastAlarmDrawAt = 0;
uint32_t lastFirmwareProgressDrawAt = 0;
uint32_t lastInteractionAt = 0;
uint32_t lastLcdRetryAt = 0;
uint32_t lastLcdHealthCheckAt = 0;
uint32_t lastLcdFaultLogAt = 0;
char toastLine[27] = "";
bool toastError = false;
uint32_t toastUntil = 0;
uint32_t wifiOfflineSinceAt = 0U;
bool wifiOfflineTracking = false;
bool wifiOfflineNoticeActive = false;
uint32_t wifiWeakSinceAt = 0U;
bool wifiWeakTracking = false;
bool wifiWeakNoticeActive = false;
uint8_t onlineServiceDegradedMaskSeen = 0U;
uint32_t lastCommandPollAt = 0;
uint32_t inputGuardUntil = 0;

HmiCommand commandQueue[COMMAND_QUEUE_SIZE];
uint8_t commandHead = 0, commandTail = 0, commandCount = 0;
uint8_t commandOutstandingCount = 0;
uint32_t nextCommandId = 1;

struct ActiveCommandSlot {
  bool used = false;
  uint32_t takenAt = 0;
  HmiCommand command;
};
ActiveCommandSlot activeCommands[COMMAND_QUEUE_SIZE];

struct CommandAck {
  uint32_t commandId = 0;
  bool ok = false;
  char message[64] = "";
};
CommandAck commandAckQueue[COMMAND_ACK_QUEUE_SIZE];
uint8_t commandAckHead = 0, commandAckTail = 0, commandAckCount = 0;

struct ConfigAckInbox {
  bool pending = false;
  uint32_t transactionId = 0;
  bool ok = false;
  bool hasStoredConfig = false;
  MachineConfig storedConfig;
};

portMUX_TYPE hmiApiMux = portMUX_INITIALIZER_UNLOCKED;
MachineRuntime runtimeInbox;
MachineConfig configInbox;
HmiEventSnapshot eventLogInbox;
bool runtimeInboxPending = false;
bool configInboxPending = false;
bool eventLogInboxPending = false;
// Man hinh khoi dong: thoat khi DA nhan du ca runtime lan config that (de man
// chinh hien ra la da day du so lieu, khong con o trong) va da qua
// SPLASH_MIN_MS; hoac het SPLASH_MAX_MS thi thoat du chua nhan duoc gi.
// (v1.1.5: KHONG con SPLASH_MAX_MS. Man khoi dong chi thoat khi boot coordinator nha Home; coordinator dung -> HMI tu hien E993.)
bool splashActive = true;
MayapBoot::SplashView splashShown = MayapBoot::SplashView::Logo;
bool splashOperatorHome = false;
uint32_t splashStartedAt = 0;
bool splashHadRuntime = false;
bool splashHadConfig = false;
bool apiWorkPending = false;
char dateInbox[11] = "--/--/----";
bool dateInboxPending = false;
BuzzerCue cueInbox = BuzzerCue::None;
bool cueInboxPending = false;
uint32_t expiredAlarmAckMask = AlarmNone;
ConfigAckInbox configAckInbox;

bool apiHasPendingWork() {
  return __atomic_load_n(&apiWorkPending, __ATOMIC_ACQUIRE);
}

void markApiWorkPending() {
  __atomic_store_n(&apiWorkPending, true, __ATOMIC_RELEASE);
}

void setApiWorkPending(bool pending) {
  __atomic_store_n(&apiWorkPending, pending, __ATOMIC_RELEASE);
}

// Prototype thu cong de Arduino .ino preprocessor khong chen khai bao
// processConfigAck truoc dinh nghia ConfigAckInbox.
void processConfigAck(const ConfigAckInbox &ack);

HmiI2cLockFn i2cLockCallback = nullptr;
HmiI2cUnlockFn i2cUnlockCallback = nullptr;

// Menu chinh: CAI DAT ME, NHAT KY ME, CAI DAT CHUNG, THOAT. "Nhat ky me" la
// man hinh XEM LICH SU (khong phai 1 "cai dat") nen hop ly hon khi dat ngang
// hang voi Cai dat chung thay vi chon sau vao trong do - menu chinh chi con
// 3 muc (Cai dat me/Cai dat chung/Thoat) bi che la "trong", dua Nhat ky me
// ra day vua giai quyet, vua dung ban chat cua no.
constexpr uint8_t MAIN_COUNT = 4;
enum MainMenuIndex : uint8_t {
  MAIN_CAI_DAT_ME = 0, MAIN_NHAT_KY = 1, MAIN_CAI_DAT_CHUNG = 2, MAIN_THOAT = 3
};
const char *mainItemLabel(uint8_t index) {
  switch (index) {
    case MAIN_CAI_DAT_ME: return "CAI DAT ME";
    case MAIN_NHAT_KY: return "LOI / NHAT KY";
    case MAIN_CAI_DAT_CHUNG: return "CAI DAT CHUNG";
    default: return "THOAT";
  }
}

// Nhom TAO AM la nhom cuoi va chi hien khi HMI da khai bao CO bo tao am.
// May khong lap phan cung se nhin y nhu truoc khi co tinh nang nay.
uint8_t visibleChungGroupCount() {
  return currentConfig.humidifierInstalled ?
      static_cast<uint8_t>(GROUP_COUNT - 1U) :
      static_cast<uint8_t>(GROUP_COUNT - 2U);
}
uint8_t chungItemCount() { return static_cast<uint8_t>(visibleChungGroupCount() + 2U); }
uint8_t chungTestIndex() { return visibleChungGroupCount(); }
const char *chungItemLabel(uint8_t index) {
  const uint8_t groupCount = visibleChungGroupCount();
  if (index < groupCount) return GROUPS[index + 1U].label;
  if (index == groupCount) return "CHE DO TEST";
  return "THOAT";
}

const char *groupExtraLabelFor(GroupExtra extra) {
  switch (extra) {
    case GroupExtra::AutoTuneEntry: return "Tu chinh PID";
    case GroupExtra::TurnStats: return "So lan dao";
    case GroupExtra::WifiChange: return "Doi wifi";
    case GroupExtra::ConnectionInfo: return "Thong tin ket noi";
    case GroupExtra::QrCode: return "Ma QR ID";
    case GroupExtra::CloudPinReset: return "Dat lai ma PIN";
    case GroupExtra::VentilationEntry: return "Thong gio";
    case GroupExtra::FirmwareWebUpdate: {
      // Danh dau "*" ngay tren ten muc khi THAT SU co ban moi dang cho -
      // nguoi dung khong can bam vao moi biet co ban moi hay khong.
      static char label[16];
      snprintf(label, sizeof(label), "Cap nhat%s",
               mayapFirmwareWebStatus().available ? " (*)" : "");
      return label;
    }
    default: return "";
  }
}
bool settingLockedDuringBatch(uint8_t settingIndex) {
  if (!currentRuntime.batchRunning && !currentRuntime.resumeConfirmationRequired) return false;
  const uint16_t offset = SETTINGS[settingIndex].offset;
  // Mo dung nhom VAN HANH co the can chinh khi dang ap. Calibration, alarm
  // safety, PID/tuning, timeout co khi va recovery/system settings bi khoa.
  if (offset == offsetof(MachineConfig, targetTemp) ||
      offset == offsetof(MachineConfig, lowHumidityAlarm) ||
      offset == offsetof(MachineConfig, humidifierEnabled) ||
      offset == offsetof(MachineConfig, targetHumidity) ||
      offset == offsetof(MachineConfig, ventOnTemp) ||
      offset == offsetof(MachineConfig, ventScheduleEnabled) ||
      offset == offsetof(MachineConfig, ventAutoEnabled) ||
      offset == offsetof(MachineConfig, ventProfileLevel) ||
      offset == offsetof(MachineConfig, ventOffTemp) ||
      offset == offsetof(MachineConfig, turningEnabled) ||
      offset == offsetof(MachineConfig, turnIntervalMin) ||
      offset == offsetof(MachineConfig, manualTurnReanchorsSchedule) ||
      offset == offsetof(MachineConfig, connectivityMode)) {
    return false;
  }
  return true;
}

uint8_t settingListItemCount(uint8_t group) {
  return static_cast<uint8_t>(
      GROUPS[group].count + groupVisibleExtraCount(group) + 1U);
}

uint8_t settingListExitIndex(uint8_t group) {
  return static_cast<uint8_t>(
      GROUPS[group].count + groupVisibleExtraCount(group));
}

bool timeReached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

void clearToast() {
  toastLine[0] = '\0';
  toastError = false;
  toastUntil = 0U;
}

void showToast(const char *text, bool error = false, uint32_t duration = 0) {
  clearToast();
  if (text && text[0]) {
    constexpr size_t maxChars = HMI_STATUS_TEXT_MAX_CHARS;
    const size_t length = strnlen(text, 64U);
    if (length <= maxChars) {
      memcpy(toastLine, text, length);
      toastLine[length] = '\0';
    } else {
      constexpr size_t body = maxChars - 3U;
      memcpy(toastLine, text, body);
      memcpy(toastLine + body, "...", 4U);
    }
  }
  toastError = error;
  toastUntil = millis() +
      (duration ? duration : (error ? TOAST_ERROR_MS : TOAST_INFO_MS));
  dirty = true;
}

void serviceOnlineHealthNotice(uint32_t now) {
  static bool unavailableTracking = false, unavailableNotice = false;
  static uint32_t unavailableSince = 0U;
  const bool unavailable = currentRuntime.connectivityMode == ConnectivityMode::Online &&
      currentRuntime.networkConnected && currentRuntime.wifiPortalState == WifiPortalState::Idle &&
      !mayapRealtimeOnline();
  if (unavailable) {
    if (!unavailableTracking) { unavailableTracking = true; unavailableSince = now; }
    if (!unavailableNotice && now - unavailableSince >= 15000U) {
      unavailableNotice = true;
      showToast("ONLINE MAT - MAY VAN CHAY", true, 8000UL);
    }
  } else {
    if (unavailableNotice && mayapRealtimeOnline()) showToast("ONLINE DA KET NOI LAI");
    unavailableTracking = unavailableNotice = false;
  }

  const bool onlineConfigured =
      currentRuntime.connectivityMode == ConnectivityMode::Online &&
      currentRuntime.networkConfigured;
  const bool portalIdle = currentRuntime.wifiPortalState == WifiPortalState::Idle;
  const bool offline = onlineConfigured && portalIdle &&
      !currentRuntime.networkConnected;

  if (offline) {
    if (!wifiOfflineTracking) {
      wifiOfflineTracking = true;
      wifiOfflineSinceAt = now;
    }
    if (!wifiOfflineNoticeActive &&
        now - wifiOfflineSinceAt >= 5000UL) {
      wifiOfflineNoticeActive = true;
      showToast("MAT WIFI - MAY VAN CHAY", true, 8000UL);
    }
  } else {
    if (wifiOfflineNoticeActive && onlineConfigured &&
        currentRuntime.networkConnected) {
      showToast("WIFI DA KET NOI LAI");
    }
    wifiOfflineTracking = false;
    wifiOfflineNoticeActive = false;
    wifiOfflineSinceAt = 0U;
  }

  const bool weak = onlineConfigured && currentRuntime.networkConnected &&
      currentRuntime.networkRssiDbm <= WIFI_RSSI_WEAK_DBM;
  if (weak) {
    if (!wifiWeakTracking) {
      wifiWeakTracking = true;
      wifiWeakSinceAt = now;
    }
    if (!wifiWeakNoticeActive &&
        now - wifiWeakSinceAt >= WIFI_RSSI_WEAK_DURATION_MS) {
      wifiWeakNoticeActive = true;
      showToast("WIFI YEU - MAY VAN CHAY", true, 8000UL);
    }
  } else {
    if (wifiWeakNoticeActive && onlineConfigured &&
        currentRuntime.networkConnected) {
      showToast("TIN HIEU WIFI DA ON");
    }
    wifiWeakTracking = false;
    wifiWeakNoticeActive = false;
    wifiWeakSinceAt = 0U;
  }

  const uint8_t degraded = mayapServiceDegradedMask();
  if (degraded != onlineServiceDegradedMaskSeen) {
    onlineServiceDegradedMaskSeen = degraded;
    if (degraded) {
      showToast("ONLINE LOI - MAY VAN CHAY", true, 8000UL);
    } else if (onlineConfigured) {
      showToast("DICH VU ONLINE DA PHUC HOI");
    }
  }
}

bool confirmationActive() {
  return confirmAction != ConfirmAction::None;
}

void resetRotaryPending(bool rearmSwallowedLongPress = false) {
  // Chi mo lai LongPress neu su kien THUC SU bi input guard nuot.
  // Khi da doi view do LongPress, phai giu co reported cho den luc nha nut;
  // neu khong mot lan giu se kich hoat tiep man vua mo.
  if (rearmSwallowedLongPress && rotary.button == ButtonEvent::LongPress &&
      rotary.stableButton == LOW) {
    rotary.longPressReported = false;
  }
  rotary.accumulator = 0;
  rotary.step = 0;
  rotary.button = ButtonEvent::None;
#if MAYAP_HMI_ENCODER_INTERRUPT
  portENTER_CRITICAL(&rotaryIsrMux);
  rotaryPendingTransitions = 0;
  portEXIT_CRITICAL(&rotaryIsrMux);
#endif
}

void armInputGuard(uint32_t durationMs = HMI_INPUT_GUARD_MS) {
  const uint32_t now = millis();
  inputGuardUntil = now + durationMs;
  // Debounce navigation only; do not schedule duplicate display frames.
  resetRotaryPending();
}

bool commandTypesConflict(HmiCommandType a, HmiCommandType b) {
  if (a == b) return true;
  const bool aBatch = a == HmiCommandType::BatchStart || a == HmiCommandType::BatchStop;
  const bool bBatch = b == HmiCommandType::BatchStart || b == HmiCommandType::BatchStop;
  if (aBatch && bBatch) return true;
  const bool aTune = a == HmiCommandType::AutoTuneStart ||
                     a == HmiCommandType::AutoTuneCancel;
  const bool bTune = b == HmiCommandType::AutoTuneStart ||
                     b == HmiCommandType::AutoTuneCancel;
  if (aTune && bTune) return true;
  if ((aTune && bBatch) || (bTune && aBatch)) return true;
  const bool aResume = a == HmiCommandType::ResumeYes || a == HmiCommandType::ResumeNo;
  const bool bResume = b == HmiCommandType::ResumeYes || b == HmiCommandType::ResumeNo;
  if (aResume && bResume) return true;
  const bool aTest = a == HmiCommandType::TestModeEnter || a == HmiCommandType::TestModeExit ||
                     a == HmiCommandType::TestOutputPulse || a == HmiCommandType::TestOutputStop ||
                     a == HmiCommandType::TestLimitStart || a == HmiCommandType::TestLimitCancel;
  const bool bTest = b == HmiCommandType::TestModeEnter || b == HmiCommandType::TestModeExit ||
                     b == HmiCommandType::TestOutputPulse || b == HmiCommandType::TestOutputStop ||
                     b == HmiCommandType::TestLimitStart || b == HmiCommandType::TestLimitCancel;
  // Thu nghiem thiet bi khong duoc dan xen voi bat dau me/auto tune.
  return (aTest && (bBatch || bTune)) || (bTest && (aBatch || aTune));
}

bool commandConflictLocked(HmiCommandType type) {
  for (uint8_t i = 0, pos = commandHead; i < commandCount;
       ++i, pos = static_cast<uint8_t>((pos + 1U) % COMMAND_QUEUE_SIZE)) {
    if (commandTypesConflict(type, commandQueue[pos].type)) return true;
  }
  for (uint8_t i = 0; i < COMMAND_QUEUE_SIZE; ++i) {
    if (activeCommands[i].used &&
        commandTypesConflict(type, activeCommands[i].command.type)) return true;
  }
  return false;
}

bool queueCommand(HmiCommandType type,
                  uint16_t validForMs = COMMAND_DEFAULT_VALID_MS,
                  uint16_t actuatorLeaseMs = 0,
                  uint32_t alarmMask = AlarmNone,
                  uint32_t *commandId = nullptr,
                  HmiCommandSource source = HmiCommandSource::Local,
                  void (*onAdmitted)(uint32_t, void *) = nullptr,
                  void *admissionContext = nullptr) {
  bool full = false;
  bool duplicate = false;
  uint32_t id = 0;
  portENTER_CRITICAL(&hmiApiMux);
  full = commandOutstandingCount >= COMMAND_QUEUE_SIZE;
  duplicate = !full && commandConflictLocked(type);
  if (!full && !duplicate) {
    id = nextCommandId++;
    if (id == 0) id = nextCommandId++;
    commandQueue[commandTail] = {
      id, type, static_cast<uint32_t>(millis()), validForMs,
      actuatorLeaseMs, alarmMask, source
    };
    // Publish correlation metadata before releasing the queue to the controller.
    if (onAdmitted) onAdmitted(id, admissionContext);
    commandTail = static_cast<uint8_t>((commandTail + 1U) % COMMAND_QUEUE_SIZE);
    ++commandCount;
    ++commandOutstandingCount;
  }
  portEXIT_CRITICAL(&hmiApiMux);

  if (full || duplicate) {
    showToast(full ? "HE THONG DANG BAN" : "LENH DANG XU LY", true);
    buzzerPlayCue(BuzzerCue::Error);
    return false;
  }
  if (commandId) *commandId = id;
  return true;
}

void finishHeaterPowerPhase(uint32_t now) {
  if (heaterTestUiPhase != HeaterTestUiPhase::Heating) return;
  (void)queueCommand(HmiCommandType::TestOutputStop, COMMAND_DEFAULT_VALID_MS,
                     0, static_cast<uint32_t>(TestOutputId::HeaterSsr));
  testModeLastCommandAt = now;
  heaterTestUiPhase = HeaterTestUiPhase::ConfirmHeat;
  heaterTestConfirmYes = true;
  armInputGuard();
  dirty = true;
}

void serviceHeaterTestWorkflow(uint32_t now) {
  if (heaterTestUiPhase == HeaterTestUiPhase::Idle) return;
  if (!currentRuntime.testModeActive) {
    heaterTestUiPhase = HeaterTestUiPhase::Idle;
    dirty = true;
    return;
  }
  if (heaterTestUiPhase == HeaterTestUiPhase::Heating) {
    const uint32_t totalMs = TEST_HEATER_FAN_PRESTART_MS + TEST_HEATER_HOLD_MAX_MS;
    const uint32_t elapsed = now - heaterTestUiStartedAt;
    const bool requestStillActive =
        (currentRuntime.testOutputMaskActive &
         (1U << static_cast<uint8_t>(TestOutputId::HeaterSsr))) != 0U;
    // 1.5 s dau cho command/control/runtime mailbox kip phan anh. Sau do neu
    // firmware da cat request vi safety thi cung chuyen sang buoc xac nhan,
    // khong de HMI dung o man "dang test" gia.
    if (elapsed >= totalMs || (elapsed >= 1500UL && !requestStillActive)) {
      finishHeaterPowerPhase(now);
      return;
    }
    if (now - heaterTestUiLastRefreshAt >= 500UL) {
      heaterTestUiLastRefreshAt = now;
      dirty = true;
    }
  }
}

void setListSelection(int value, uint8_t count) {
  if (count == 0) return;
  listIndex = static_cast<uint8_t>(constrain(value, 0, count - 1));
  if (listIndex < listTop) listTop = listIndex;
  if (listIndex >= listTop + 4) listTop = listIndex - 3;
  dirty = true;
}

void alignMainMenuWindow() {
  listTop = mainIndex >= 3 ? static_cast<uint8_t>(mainIndex - 3) : 0;
}
void alignChungMenuWindow() {
  chungTop = chungIndex >= 3 ? static_cast<uint8_t>(chungIndex - 3) : 0;
}

void goBack() {
  switch (view) {
    case View::Home: break;
    case View::MainMenu: view = View::Home; break;
    case View::ChungMenu:
      view = View::MainMenu;
      mainIndex = MAIN_CAI_DAT_CHUNG;
      alignMainMenuWindow();
      break;
    case View::SettingList:
      if (selectedGroup == 0U) {
        view = View::MainMenu;
        mainIndex = MAIN_CAI_DAT_ME;
        alignMainMenuWindow();
      } else {
        view = View::ChungMenu;
        chungIndex = static_cast<uint8_t>(selectedGroup - 1U);
        alignChungMenuWindow();
      }
      break;
    case View::EditSetting:
      view = editReturnView;
      break;
    case View::VentilationMenu:
      view = View::SettingList;
      selectedGroup = 6U;
      setListSelection(GROUPS[6U].count, settingListItemCount(6U));
      break;
    case View::VentilationAdvanced:
      view = View::VentilationMenu;
      ventilationIndex = currentConfig.ventAutoEnabled ? 2U : 1U;
      break;
    case View::FirmwareMenu:
      view = View::SettingList;
      selectedGroup = 3U;
      setListSelection(static_cast<int>(GROUPS[3U].count) +
                            visibleExtraIndexOf(3U, GroupExtra::FirmwareWebUpdate),
                        settingListItemCount(3U));
      break;
    case View::TurnStats:
      view = View::SettingList;
      selectedGroup = 2U;  // DAO TRUNG
      setListSelection(GROUPS[2U].count, settingListItemCount(2U));
      break;
    case View::AutoTune:
      // "Tu chinh PID" gio la dong phu cua nhom NHIET DO (khong con la muc
      // rieng trong ChungMenu) - quay ve dung dong nay trong SettingList.
      view = View::SettingList;
      selectedGroup = 1U;  // NHIET DO
      setListSelection(static_cast<int>(GROUPS[1U].count) +
                            visibleExtraIndexOf(1U, GroupExtra::AutoTuneEntry),
                        settingListItemCount(1U));
      break;
    case View::EventLog:
      view = eventLogReturnView;
      if (view == View::MainMenu) {
        mainIndex = MAIN_NHAT_KY;
        alignMainMenuWindow();
      }
      break;
    case View::TestMode:
    case View::TestSummary:
      queueCommand(HmiCommandType::TestModeExit);
      view = View::ChungMenu;
      chungIndex = chungTestIndex();  // "CHE DO TEST"
      alignChungMenuWindow();
      break;
    case View::WifiChange:
      queueCommand(HmiCommandType::WifiPortalCancel);
      view = View::SettingList;
      selectedGroup = 3U;  // HE THONG
      // Dung ham tra cuu thay vi hardcode vi tri - tranh sai khi thu tu
      // groupExtraSlot() thay doi sau nay (da tung sai vi ly do nay).
      setListSelection(static_cast<int>(GROUPS[3U].count) +
                            visibleExtraIndexOf(3U, GroupExtra::WifiChange),
                        settingListItemCount(3U));
      break;
    case View::ConnectionInfo:
      view = View::SettingList;
      selectedGroup = 3U;  // HE THONG
      setListSelection(static_cast<int>(GROUPS[3U].count) +
                            visibleExtraIndexOf(3U, GroupExtra::ConnectionInfo),
                        settingListItemCount(3U));
      break;
    case View::QrCode:
      view = View::SettingList;
      selectedGroup = 3U;  // HE THONG
      setListSelection(static_cast<int>(GROUPS[3U].count) +
                            visibleExtraIndexOf(3U, GroupExtra::QrCode),
                        settingListItemCount(3U));
      break;
    case View::Alarm: view = alarmReturnView; break;
    case View::TurnStatus: view = turnStatusReturnView; break;
  }
  dirty = true;
}

void openGroup(uint8_t group) {
  if (configSave.active) {
    showToast("DANG CHO XAC NHAN LUU", true);
    return;
  }
  if (currentRuntime.autoTuneState == AutoTuneState::Running) {
    showToast("AUTO TUNE DANG CHAY", true);
    return;
  }
  selectedGroup = group;
  listIndex = listTop = 0;
  view = View::SettingList;
  dirty = true;
}

void openChungMenu() {
  if (currentRuntime.autoTuneState == AutoTuneState::Running) {
    showToast("AUTO TUNE DANG CHAY", true);
    return;
  }
  chungIndex = chungTop = 0;
  view = View::ChungMenu;
  dirty = true;
}

void selectChungItem() {
  const uint8_t groupCount = visibleChungGroupCount();
  if (chungIndex < groupCount) {
    openGroup(static_cast<uint8_t>(chungIndex + 1U));
  } else if (chungIndex == groupCount) {
    // CHE DO TEST - openTestMode() tu kiem tra dieu kien + dat dirty.
    openTestMode();
  } else {
    view = View::MainMenu;
    mainIndex = MAIN_CAI_DAT_CHUNG;
    alignMainMenuWindow();
    dirty = true;
  }
}

// "Tu chinh PID" - dong phu cua nhom NHIET DO (xem groupExtraSlot()). Giu
// nguyen dieu kien chan cu (khong the tu chinh PID trong luc dang co me).
void openAutoTuneEntry() {
  if (currentRuntime.batchRunning) {
    showToast("HAY DUNG ME TRUOC AUTO TUNE", true);
    return;
  }
  view = View::AutoTune;
  dirty = true;
}

void openTestMode() {
  if (currentRuntime.batchRunning) {
    showToast("DANG CO ME - KHONG TEST DUOC", true);
    return;
  }
  if (currentRuntime.autoTuneState == AutoTuneState::Running) {
    showToast("AUTO TUNE DANG CHAY", true);
    return;
  }
  if (queueCommand(HmiCommandType::TestModeEnter)) {
    testModeLastCommandAt = millis();
    view = View::TestMode;
    listIndex = listTop = 0;
    testDeviceConfirmActive = false;
    for (auto &r : testDeviceResult) r = TestResult::Untested;
    for (auto &r : testLimitResult) r = TestResult::Untested;
    lastObservedTestLimitPhase = TestLimitPhase::Idle;
    dirty = true;
  }
}

void openWifiChange() {
  if (currentConfig.connectivityMode != ConnectivityMode::Online) {
    showToast("CHI DUNG DUOC KHI ONLINE", true);
    return;
  }
  if (queueCommand(HmiCommandType::WifiPortalStart)) {
    wifiPortalLastCommandAt = millis();
    view = View::WifiChange;
    dirty = true;
  }
}

void openSettingIndex(uint8_t settingIndex, View returnView) {
  editSettingIndex = settingIndex;
  editReturnView = returnView;
  if (settingLockedDuringBatch(editSettingIndex)) {
    showToast("DANG AP - THONG SO BI KHOA", true);
    return;
  }
  if (configSave.active) {
    showToast("DANG LUU - VUI LONG CHO", true);
    return;
  }
  const SettingItem &item = SETTINGS[editSettingIndex];
  float minimum, maximum;
  settingLimits(currentConfig, item, minimum, maximum);
  editValue = constrain(readSetting(currentConfig, item), minimum, maximum);
  view = View::EditSetting;
  dirty = true;
}

void openSetting() {
  const SettingGroup &group = GROUPS[selectedGroup];
  openSettingIndex(GROUP_SETTING_INDEXES[group.first + listIndex],
                   View::SettingList);
}

void openBatchConfirm(View returnView) {
  // Dang chay me thi luon cho phep mo xac nhan DUNG ME, ke ca EEPROM dang
  // hoan tat giao dich luu. Chi chan thao tac BAT DAU me moi.
  if (configSave.active && !currentRuntime.batchRunning) {
    showToast("CHO LUU XONG TRUOC KHI CHAY ME", true);
    return;
  }
  if (currentRuntime.autoTuneState == AutoTuneState::Running) {
    showToast("HAY CHO AUTO TUNE XONG", true);
    return;
  }
  confirmAction = ConfirmAction::BatchToggle;
  confirmReturnView = returnView;
  // Bat dau: mac dinh DONG Y de thao tac nhanh.
  // Dung me: mac dinh HUY de tranh dung nham chu trinh dang chay.
  confirmYes = !currentRuntime.batchRunning;
  clearToast();
  armInputGuard();
  dirty = true;
}

// Bat/tat "Dao tu dong" gio hoi CO/HUY truoc khi ap dung, giong het thao tac
// BAT DAU/DUNG ME - vi day la thay doi anh huong truc tiep den viec dao
// trung dang chay, khong nen ap ngay chi tu 1 lan xoay/bam nham. `candidate`
// da duoc tinh san (gia tri turningEnabled moi) boi commitSetting().
void openTurningToggleConfirm(const MachineConfig &candidate, View returnView) {
  pendingTurningConfig = candidate;
  confirmAction = ConfirmAction::TurningToggle;
  confirmReturnView = returnView;
  // Bat dao: mac dinh DONG Y. Tat dao giua luc dang co the dang ap: mac dinh
  // HUY de tranh vo tinh tat mat dao trung.
  confirmYes = candidate.turningEnabled;
  clearToast();
  armInputGuard();
  dirty = true;
}


void openResumeConfirm() {
  confirmAction = ConfirmAction::ResumeBatch;
  confirmReturnView = View::Home;
  confirmYes = true;
  view = View::Home;
  homePage = 0;
  clearToast();
  armInputGuard();
  dirty = true;
}

void openBatchOverdueContinueConfirm() {
  confirmAction = ConfirmAction::BatchOverdueContinue;
  confirmReturnView = View::Home;
  // Mac dinh CO (tiep tuc u am) - giong tinh than ResumeBatch: khong tu y
  // suy dien "muon dung" chi tu 1 lan xoay/bam nham, phai chu dong chon HUY.
  confirmYes = true;
  view = View::Home;
  homePage = 0;
  clearToast();
  armInputGuard();
  dirty = true;
}

void openAutoTuneConfirm() {
  if (configSave.active) {
    showToast("CHO LUU XONG TRUOC", true);
    return;
  }
  if (currentRuntime.autoTuneState == AutoTuneState::Running) {
    confirmAction = ConfirmAction::AutoTuneCancel;
    confirmReturnView = View::AutoTune;
    confirmYes = false;
    clearToast();
    armInputGuard();
    dirty = true;
    return;
  }
  if (currentRuntime.batchRunning) {
    showToast("HAY DUNG ME TRUOC AUTO TUNE", true);
    return;
  }
  if (!currentRuntime.sensorOnline ||
      (currentRuntime.alarmMask & (AlarmSensor | AlarmEmergency | AlarmSystem))) {
    showToast("CAM BIEN/AN TOAN CHUA SAN SANG", true);
    return;
  }
  confirmAction = ConfirmAction::AutoTuneStart;
  confirmReturnView = View::AutoTune;
  confirmYes = false;
  clearToast();
  armInputGuard();
  dirty = true;
}

// Dat lai ma PIN web (dung khi them thiet bi/doi ten may tren dashboard)
// va tao mot ma ngau nhien moi. Day la duong duy nhat de khoi phuc quyen
// truy cap neu nguoi dung da doi PIN roi quen - co mat vat ly tai HMI (bam
// nut xac nhan CO) la dieu kien duy nhat, khong can biet PIN cu.
void openCloudPinResetConfirm() {
  if (currentConfig.connectivityMode != ConnectivityMode::Online) {
    showToast("CHI DUNG DUOC KHI ONLINE", true);
    return;
  }
  confirmAction = ConfirmAction::CloudPinReset;
  confirmReturnView = View::SettingList;
  confirmYes = false;
  clearToast();
  armInputGuard();
  dirty = true;
}

// Quay lai firmware truoc do (ota_rollback.h) - KHONG can Online (thao tac
// hoan toan noi bo, chi doi con tro vi tri khoi dong trong flash). Muc nay
// chi hien khi mayapRollbackAvailable() dung (xem groupExtraSlotVisible()),
// nen den day chac chan con ban de quay lai - van kiem tra lai LAN CUOI
// thuc su luc ap dung (mayapFirmwareRollbackUpdate()) phong truong hop
// hiem gap.
void openFirmwareRollbackConfirm() {
  confirmAction = ConfirmAction::FirmwareRollback;
  confirmReturnView = View::FirmwareMenu;
  confirmYes = false;
  clearToast();
  armInputGuard();
  dirty = true;
}

// Cap nhat firmware TU XA (ota_web_update.h) - muc nay LUON bam vao duoc khi
// Online (xem groupExtraSlotVisible()): neu THAT SU co ban moi dang cho thi
// mo man hinh xac nhan Co/Khong; neu chua co thi chi hien thong tin phien
// ban dang chay de nguoi dung biet may minh dang o ban nao, khong lam gi
// them ca. Bam CO moi thuc su tai ve/flash - xem executeConfirmation().
void openFirmwareWebConfirm() {
  if (currentConfig.connectivityMode != ConnectivityMode::Online) {
    showToast("CHI DUNG DUOC KHI ONLINE", true);
    return;
  }
  if (!mayapFirmwareWebStatus().available) {
    if (queueCommand(HmiCommandType::FirmwareWebCheckNow))
      showToast("DANG KIEM TRA BAN MOI");
    return;
  }
  confirmAction = ConfirmAction::FirmwareWebApply;
  // Dung VIEW HIEN TAI (khong ep cung "SettingList"): ham nay duoc goi tu
  // 2 noi - bam tay trong SettingList (tra ve do khi bam Khong la dung), VA
  // tu dong tren man Home do nhac dinh ky (xem doan kiem tra firmware
  // reminder trong applyRuntime(), ngay sau doan resumeConfirmationRequired)
  // - luc do phai tra ve Home chu khong "nhay lac" nguoi dung vao menu cai
  // dat ma ho khong yeu cau.
  confirmReturnView = view;
  confirmYes = false;
  clearToast();
  armInputGuard();
  dirty = true;
}

constexpr uint32_t ALARM_PRIORITY[] = {
  AlarmEmergency, AlarmSystem, AlarmAutoMode, AlarmSensor, AlarmTurning,
  AlarmTempHigh, AlarmTempLow, AlarmHumidityLow
};
constexpr uint8_t ALARM_PRIORITY_COUNT = sizeof(ALARM_PRIORITY) / sizeof(ALARM_PRIORITY[0]);

// ============================================================
// 3A. BO DIEU KHIEN COI KHONG BLOCKING
// ============================================================
void buzzerWrite(bool on) {
  buzzer.outputOn = on;
  digitalWrite(PIN_BUZZER, (on == BUZZER_ACTIVE_HIGH) ? HIGH : LOW);
}

void buzzerBegin() {
  // Dat muc OFF truoc khi OUTPUT de tranh mot xung keu luc khoi dong.
  digitalWrite(PIN_BUZZER, BUZZER_ACTIVE_HIGH ? LOW : HIGH);
  pinMode(PIN_BUZZER, OUTPUT);
  buzzerWrite(false);
}

// "Giai dieu" khoi dong: coi la loai active (xem ghi chu tren) nen KHONG the
// doi cao do - "giai dieu" o day la mot NHIP rieng (do dai bip/khoang lang
// tang dan) de nghe khac han moi tin hieu coi khac (Key/Save/Ok/Error/canh
// bao), phat DUNG 1 LAN luc bat may. Ngan (~800ms) theo dung yeu cau.
constexpr uint8_t STARTUP_CHIME_STEPS = 7U;
constexpr bool STARTUP_CHIME_ON[STARTUP_CHIME_STEPS] =
    {true, false, true, false, true, false, true};
constexpr uint16_t STARTUP_CHIME_MS[STARTUP_CHIME_STEPS] =
    {70, 70, 70, 70, 140, 90, 280};

void buzzerPlayStartupChime() {
  buzzer.startupActive = true;
  buzzer.startupPhase = 0U;
  buzzerWrite(true);
  buzzer.deadline = millis() + STARTUP_CHIME_MS[0];
}

BuzzerPattern buzzerCuePattern(BuzzerCue cue) {
  switch (cue) {
    // Coi la loai active (tu dao dong, khong chinh duoc cao do/am luong qua
    // PWM), nen cach duy nhat de "diu" tieng la rut ngan thoi gian xung. Key
    // vang o moi lan bam nen phai la tieng "tach" that nhe, khong phai tieng "bip".
    // pulses=0 -> buzzerPlayCue() tu bo qua (xem "if (!pattern.pulses) return;")
    // - bo han tieng khi nhan phim theo yeu cau, khong can dung tung noi goi.
    case BuzzerCue::Key:   return {0, 0, 0};
    case BuzzerCue::Save:  return {10, 0, 1};      // Bip cuc ngan, khong gay on
    case BuzzerCue::Ok:    return {35, 50, 2};     // Hai bip nhe
    case BuzzerCue::Error: return {90, 90, 3};     // Van giu 3 bip ro rang de phan biet loi (an toan)
    default:               return {0, 0, 0};
  }
}

BuzzerPattern alarmPattern(uint32_t bit) {
  // Coi HMI la loai active (tu dao dong san, xem ghi chu o buzzerCuePattern())
  // nen KHONG chinh duoc do to nho/dB qua phan mem - da rut ngan het muc thoi
  // gian xung (onMs) con lai gan mot nua so voi truoc, giam tong thoi gian
  // keu that su moi lan bao (van du de nghe thay va phan biet cac muc do).
  switch (bit) {
    case AlarmEmergency:   return {450, 100, 0};   // Gan lien tuc, uu tien cao nhat
    case AlarmSystem:      return {300, 200, 0};
    case AlarmAutoMode:    return {150, 700, 0};
    case AlarmSensor:      return {220, 300, 0};
    case AlarmTurning:     return {200, 400, 0};
    case AlarmTempHigh:    return {175, 500, 0};
    case AlarmTempLow:     return {110, 900, 0};
    case AlarmHumidityLow: return {60, 2880, 0};  // Het nuoc: nhac nho thua
    default:               return {0, 0, 0};
  }
}

uint32_t highestUnacknowledgedAlarm() {
  const uint32_t pending = currentRuntime.alarmMask & ~buzzer.acknowledgedAlarmMask;
  for (uint8_t i = 0; i < ALARM_PRIORITY_COUNT; ++i) {
    if (pending & ALARM_PRIORITY[i]) return ALARM_PRIORITY[i];
  }
  return AlarmNone;
}

void buzzerStopTransient() {
  buzzer.transientActive = false;
  buzzer.transientOn = false;
  buzzer.transientMandatory = false;
  buzzer.transientPulsesLeft = 0;
}

void buzzerPlayCue(BuzzerCue cue) {
  // Bao dong va yeu cau phuc hoi co uu tien cao hon tieng UI.
  if (highestUnacknowledgedAlarm() != AlarmNone ||
      currentRuntime.resumeConfirmationRequired ||
      currentRuntime.turnState == TurnState::Left ||
      currentRuntime.turnState == TurnState::Right) return;
  const BuzzerPattern pattern = buzzerCuePattern(cue);
  if (!pattern.pulses) return;
  const bool mandatory = cue == BuzzerCue::Error;
  if (!currentConfig.alarmEnabled && !mandatory) return;
  buzzer.transientPattern = pattern;
  buzzer.transientPulsesLeft = pattern.pulses;
  buzzer.transientActive = true;
  buzzer.transientOn = true;
  buzzer.transientMandatory = mandatory;
  buzzer.deadline = millis() + pattern.onMs;
  buzzerWrite(true);
}

void buzzerAcknowledge(uint32_t alarmMask) {
  const uint32_t now = millis();
  alarmMask &= ALARM_KNOWN_MASK;
  buzzer.acknowledgedAlarmMask |= alarmMask;
  for (uint8_t i = 0; i < ALARM_PRIORITY_COUNT; ++i) {
    if (alarmMask & ALARM_PRIORITY[i]) buzzer.acknowledgedAt[i] = now;
  }
  if (alarmMask & AlarmTempHigh) {
    // Moc goc cho "coi thong minh" (xem buzzerUpdate()) - lay nhiet do ngay
    // luc ACK lam moc so sanh dau tien.
    buzzer.tempHighAckBestTemp = (currentRuntime.sensorOnline &&
                                  isfinite(currentRuntime.temperature))
        ? currentRuntime.temperature : NAN;
    buzzer.tempHighAckLastSampleAt = now;
  }
  buzzer.soundingAlarmBit = AlarmNone;
  buzzerStopTransient();
  buzzerWrite(false);
}

void buzzerUnacknowledge(uint32_t alarmMask) {
  buzzer.acknowledgedAlarmMask &= ~alarmMask;
  for (uint8_t i = 0; i < ALARM_PRIORITY_COUNT; ++i) {
    if (alarmMask & ALARM_PRIORITY[i]) buzzer.acknowledgedAt[i] = 0;
  }
  if (alarmMask & AlarmTempHigh) {
    buzzer.tempHighAckBestTemp = NAN;
    buzzer.tempHighAckLastSampleAt = 0;
  }
  buzzer.soundingAlarmBit = AlarmNone;
}

// LUU Y: AlarmTempHigh KHONG dung ham nay - no co logic rieng theo doi xu
// huong nhiet ("coi thong minh", xem nhanh rieng trong buzzerUpdate()) thay
// vi hen gio co dinh.
uint32_t alarmRepeatMs(uint32_t bit) {
  if (bit == AlarmEmergency) return EMERGENCY_RESOUND_MS;
  if (bit == AlarmAutoMode) return AUTO_LOST_RESOUND_MS;
  if (bit == AlarmSystem || bit == AlarmSensor || bit == AlarmTurning) {
    return CRITICAL_RESOUND_MS;
  }
  return 0;
}

void buzzerUpdate(uint32_t now) {
  // Giai dieu khoi dong duoc uu tien tuyet doi trong luc con hieu luc, NHUNG
  // luon dung ngay khi roi man khoi dong (vao View::Home) - khong bao gio de no lan sang canh bao that hoac coi thao
  // tac binh thuong.
  if (buzzer.startupActive) {
    if (!splashActive) {
      buzzer.startupActive = false;
      buzzerWrite(false);
    } else if (static_cast<int32_t>(now - buzzer.deadline) >= 0) {
      const uint8_t next = static_cast<uint8_t>(buzzer.startupPhase + 1U);
      if (next >= STARTUP_CHIME_STEPS) {
        buzzer.startupActive = false;
        buzzerWrite(false);
      } else {
        buzzer.startupPhase = next;
        buzzerWrite(STARTUP_CHIME_ON[next]);
        buzzer.deadline = now + STARTUP_CHIME_MS[next];
      }
    }
    return;
  }
  // Bit da het loi tu dong mat ACK. Loi tai xuat hien se keu lai.
  buzzer.acknowledgedAlarmMask &= currentRuntime.alarmMask;
  if (!(buzzer.acknowledgedAlarmMask & AlarmTempHigh)) {
    buzzer.tempHighAckBestTemp = NAN;
    buzzer.tempHighAckLastSampleAt = 0;
  }
  for (uint8_t i = 0; i < ALARM_PRIORITY_COUNT; ++i) {
    const uint32_t bit = ALARM_PRIORITY[i];
    if (!(buzzer.acknowledgedAlarmMask & bit)) {
      buzzer.acknowledgedAt[i] = 0;
      continue;
    }
    if (bit == AlarmTempHigh) {
      // "Coi thong minh": khac moi bit khac (hen gio co dinh CRITICAL_
      // RESOUND_MS ben duoi), rieng nhiet do cao theo doi XU HUONG - con dang
      // giam thi giu im (ma loi E111 van hien tren man hinh binh thuong,
      // khong bi xoa), dung yen hoac tang tro lai thi keu lai NGAY khong doi
      // het gio. Mat cam bien giua luc dang im lang thi khong the biet xu
      // huong nua - an toan hon la keu lai ngay.
      const bool haveTemp = currentRuntime.sensorOnline &&
                            isfinite(currentRuntime.temperature);
      bool resoundNow = !haveTemp;
      if (haveTemp && timeReached(now, buzzer.tempHighAckLastSampleAt +
                                       TEMP_HIGH_SMART_MUTE_SAMPLE_MS)) {
        buzzer.tempHighAckLastSampleAt = now;
        if (!isfinite(buzzer.tempHighAckBestTemp)) {
          buzzer.tempHighAckBestTemp = currentRuntime.temperature;
        } else if (currentRuntime.temperature <=
                   buzzer.tempHighAckBestTemp - TEMP_HIGH_SMART_MUTE_EPSILON_C) {
          buzzer.tempHighAckBestTemp = currentRuntime.temperature;  // van giam that
        } else {
          resoundNow = true;  // dung yen hoac tang tro lai
        }
      }
      if (resoundNow) {
        buzzer.acknowledgedAlarmMask &= ~bit;
        buzzer.acknowledgedAt[i] = 0;
        alarmPresentedMask &= ~bit;
        buzzer.tempHighAckBestTemp = NAN;
        buzzer.tempHighAckLastSampleAt = 0;
      }
      continue;
    }
    const uint32_t repeatMs = alarmRepeatMs(bit);
    if (repeatMs && now - buzzer.acknowledgedAt[i] >= repeatMs) {
      buzzer.acknowledgedAlarmMask &= ~bit;
      buzzer.acknowledgedAt[i] = 0;
      // Re-arming the buzzer also starts a new presentation cycle: the Alarm
      // screen must return instead of leaving an audible-only repeat.
      alarmPresentedMask &= ~bit;
    }
  }

  const uint32_t alarmBit = highestUnacknowledgedAlarm();
  // Moi loi/canh bao moi deu phat am it nhat den khi nguoi dung ACK.
  // alarmEnabled chi tat cac bip giao dien thong thuong, khong che giau loi.
  if (alarmBit != AlarmNone) {
    buzzer.resumePromptActive = false;
    buzzer.turnPatternActive = false;
    buzzer.turnPhase = 0U;
    buzzerStopTransient();

    if (alarmBit == AlarmEmergency) {
      // Nhip "Temporal-3" (ISO 8201) - chuan quoc te cho coi bao chay/khan
      // cap: 3 tieng bip ngan + 1 khoang lang dai hon, lap lai. Nghe RO la
      // canh bao that su ngay lap tuc, thay vi 1 tieng bip don deu deu nhu
      // cac muc canh bao khac (van giu nguyen alarmPattern() cho cac muc do
      // thap hon - chi rieng Khan cap moi dang nhip nay).
      constexpr uint8_t T3_STEPS = 6U;
      constexpr bool T3_ON[T3_STEPS]      = {true, false, true, false, true, false};
      constexpr uint16_t T3_MS[T3_STEPS]  = {450, 450, 450, 450, 450, 1300};
      if (buzzer.soundingAlarmBit != alarmBit) {
        buzzer.soundingAlarmBit = alarmBit;
        buzzer.emergencyPhase = 0U;
        buzzerWrite(true);
        buzzer.deadline = now + T3_MS[0];
        return;
      }
      if (static_cast<int32_t>(now - buzzer.deadline) < 0) return;
      buzzer.emergencyPhase = static_cast<uint8_t>((buzzer.emergencyPhase + 1U) % T3_STEPS);
      buzzerWrite(T3_ON[buzzer.emergencyPhase]);
      buzzer.deadline = now + T3_MS[buzzer.emergencyPhase];
      return;
    }

    const BuzzerPattern pattern = alarmPattern(alarmBit);
    if (buzzer.soundingAlarmBit != alarmBit) {
      buzzer.soundingAlarmBit = alarmBit;
      buzzerWrite(true);
      buzzer.deadline = now + pattern.onMs;
      return;
    }
    if (static_cast<int32_t>(now - buzzer.deadline) < 0) return;
    if (buzzer.outputOn) {
      buzzerWrite(false);
      buzzer.deadline = now + pattern.offMs;
    } else {
      buzzerWrite(true);
      buzzer.deadline = now + pattern.onMs;
    }
    return;
  }

  buzzer.soundingAlarmBit = AlarmNone;

  // Mat dien giua me la yeu cau bat buoc nguoi dung ra quyet dinh.
  // Coi nhac lap lai den khi chon TIEP TUC hoac HUY ME.
  if (currentRuntime.resumeConfirmationRequired) {
    buzzer.turnPatternActive = false;
    buzzer.turnPhase = 0U;
    buzzerStopTransient();
    if (!buzzer.resumePromptActive) {
      buzzer.resumePromptActive = true;
      buzzerWrite(true);
      buzzer.deadline = now + RESUME_PROMPT_ON_MS;
      return;
    }
    if (static_cast<int32_t>(now - buzzer.deadline) < 0) return;
    if (buzzer.outputOn) {
      buzzerWrite(false);
      buzzer.deadline = now + RESUME_PROMPT_OFF_MS;
    } else {
      buzzerWrite(true);
      buzzer.deadline = now + RESUME_PROMPT_ON_MS;
    }
    return;
  }
  buzzer.resumePromptActive = false;

  // Khi motor dao dang chay, coi phu phat hai bip ngan theo chu ky.
  // Nhip nay de nhan biet dao, khong bi nham voi canh bao va khong keu lien.
  const bool turningNow = currentRuntime.turnState == TurnState::Left ||
                          currentRuntime.turnState == TurnState::Right;
  if (turningNow) {
    buzzerStopTransient();
    if (!buzzer.turnPatternActive) {
      buzzer.turnPatternActive = true;
      buzzer.turnPhase = 0U;
      buzzerWrite(true);
      buzzer.deadline = now + TURN_BUZZER_ON1_MS;
      return;
    }
    if (static_cast<int32_t>(now - buzzer.deadline) < 0) return;
    switch (buzzer.turnPhase) {
      case 0U:
        buzzerWrite(false);
        buzzer.turnPhase = 1U;
        buzzer.deadline = now + TURN_BUZZER_GAP_MS;
        break;
      case 1U:
        buzzerWrite(true);
        buzzer.turnPhase = 2U;
        buzzer.deadline = now + TURN_BUZZER_ON2_MS;
        break;
      case 2U:
        buzzerWrite(false);
        buzzer.turnPhase = 3U;
        buzzer.deadline = now + TURN_BUZZER_PAUSE_MS;
        break;
      default:
        buzzerWrite(true);
        buzzer.turnPhase = 0U;
        buzzer.deadline = now + TURN_BUZZER_ON1_MS;
        break;
    }
    return;
  }
  if (buzzer.turnPatternActive) {
    buzzer.turnPatternActive = false;
    buzzer.turnPhase = 0U;
    buzzerWrite(false);
  }

  if (!buzzer.transientActive ||
      (!currentConfig.alarmEnabled && !buzzer.transientMandatory)) {
    buzzerStopTransient();
    buzzerWrite(false);
    return;
  }

  if (static_cast<int32_t>(now - buzzer.deadline) < 0) return;
  if (buzzer.transientOn) {
    buzzerWrite(false);
    buzzer.transientOn = false;
    if (buzzer.transientPulsesLeft > 0) --buzzer.transientPulsesLeft;
    if (!buzzer.transientPulsesLeft) {
      buzzerStopTransient();
      return;
    }
    buzzer.deadline = now + buzzer.transientPattern.offMs;
  } else {
    buzzerWrite(true);
    buzzer.transientOn = true;
    buzzer.deadline = now + buzzer.transientPattern.onMs;
  }
}

void openAlarmView(View returnView) {
  if (!currentRuntime.activeFaultDisplayCount) return;
  alarmReturnView = returnView == View::Alarm ? View::Home : returnView;
  alarmIndex = 0;
  view = View::Alarm;
  dirty = true;
}

bool startConfigSave(const MachineConfig &candidate, bool deferForHost = false,
                     uint32_t *transactionId = nullptr) {
  if (configSave.active) {
    showToast("DANG CHO XAC NHAN LUU", true);
    return false;
  }
  uint32_t id = nextConfigTransactionId++;
  if (id == 0) id = nextConfigTransactionId++;
  portENTER_CRITICAL(&hmiApiMux);
  configSave.active = true;
  configSave.readyForHost = !deferForHost;
  configSave.id = id;
  if (transactionId) *transactionId = id;
  configSave.startedAt = millis();
  configSave.rollback = currentConfig;
  configSave.candidate = candidate;
  portEXIT_CRITICAL(&hmiApiMux);
  return true;
}


void commitSetting() {
  if (settingLockedDuringBatch(editSettingIndex)) {
    view = editReturnView;
    showToast("DANG AP - KHONG DUOC DOI", true);
    return;
  }
  MachineConfig candidate = currentConfig;
  const SettingItem &item = SETTINGS[editSettingIndex];
  float minimum, maximum;
  settingLimits(currentConfig, item, minimum, maximum);
  const float oldTarget = candidate.targetTemp;
  writeSetting(candidate, item, constrain(editValue, minimum, maximum));
  if (currentRuntime.batchRunning &&
      item.offset == offsetof(MachineConfig, connectivityMode) &&
      currentConfig.connectivityMode == ConnectivityMode::Offline &&
      candidate.connectivityMode == ConnectivityMode::Online) {
    view = editReturnView;
    showToast("DANG AP - KHONG BAT ONLINE", true);
    return;
  }
  if (item.offset == offsetof(MachineConfig, targetTemp)) {
    // Khi doi SV, giu nguyen khoang cach cac nguong so voi SV.
    // Nguoi van hanh khong phai chinh lai tung muc lien quan.
    const float delta = candidate.targetTemp - oldTarget;
    candidate.lowTempAlarm += delta;
    candidate.highTempAlarm += delta;
    candidate.emergencyTemp += delta;
    candidate.ventOnTemp += delta;
    candidate.ventOffTemp += delta;
  }
  sanitizeConfig(candidate);

  const float oldValue = readSetting(currentConfig, item);
  const float newValue = readSetting(candidate, item);
  view = editReturnView;
  // Gia tri khong doi thi thoat lang le, khong can bao gi ca - nguoi dung
  // chi xem/luot qua thong so thi khong nen bi lam phien bang 1 dong toast.
  if (fabsf(oldValue - newValue) < 0.0001f) return;
  // Bat/tat "Dao tu dong" hoi CO/HUY truoc khi ap dung (giong Bat dau/Dung
  // me) thay vi luu ngay - danh gia sai lech co the lam mat dao trung giua
  // luc dang ap ma khong ai de y.
  if (item.offset == offsetof(MachineConfig, turningEnabled)) {
    openTurningToggleConfirm(candidate, editReturnView);
    return;
  }
  if (!startConfigSave(candidate)) return;
  currentConfig = candidate;
  showToast("DANG LUU...");
}

void exitSettingGroup() {
  if (selectedGroup == 0U) {
    view = View::MainMenu;
    mainIndex = MAIN_CAI_DAT_ME;
    alignMainMenuWindow();
  } else {
    view = View::ChungMenu;
    chungIndex = static_cast<uint8_t>(selectedGroup - 1U);
    alignChungMenuWindow();
  }
  dirty = true;
}

bool requestAlarmAcknowledge() {
  const uint32_t snapshot = currentRuntime.alarmMask & ALARM_KNOWN_MASK;
  if (!snapshot) {
    showToast("KHONG CO CANH BAO");
    return false;
  }
  uint32_t commandId = 0;
  if (!queueCommand(HmiCommandType::AlarmAck, COMMAND_DEFAULT_VALID_MS,
                    0, snapshot, &commandId)) return false;
  buzzerAcknowledge(snapshot);
  showToast("DA TAT COI - LOI VAN CON");
  return true;
}

void selectMainItem() {
  switch (mainIndex) {
    case MAIN_CAI_DAT_ME: openGroup(0U); break;
    case MAIN_NHAT_KY:
      eventLogIndex = 0U;
      eventLogFaultsOnly = false;
      eventLogReturnView = View::MainMenu;
      view = View::EventLog;
      break;
    case MAIN_CAI_DAT_CHUNG: openChungMenu(); break;
    default:
      view = View::Home;
      homePage = 0;
      break;
  }
  dirty = true;
}

// Quy tac HMI khi dang co loi:
// - Nhan ngan: giu nguyen chuc nang cua trang dang dung. Neu man canh bao
//   dang tu bat, nhan ngan ACK/tat coi va an man nay tam thoi.
// - Nhan giu tai Home: xem loi dang hoat dong, neu da het thi xem loi gan day.
// Nhieu loi van duoc xu ly o MachineController; doi thao tac HMI khong lam
// vo hieu bat ky bao ve an toan nao.
void activateHomeContext(bool longPress) {
  if (longPress) {
    if (currentRuntime.activeFaultDisplayCount) {
      openAlarmView(View::Home);
    } else {
      eventLogIndex = 0U;
      eventLogFaultsOnly = true;
      eventLogReturnView = View::Home;
      view = View::EventLog;
      dirty = true;
    }
    return;
  }
  if (homePage == 0U) {
    openBatchConfirm(View::Home);
  } else {
    view = View::MainMenu;
    mainIndex = 0U;
    listTop = 0U;
    dirty = true;
  }
}

uint8_t ventilationMenuCount() {
  return currentConfig.ventAutoEnabled ? 4U : 2U;
}
uint8_t firmwareMenuCount() {
  return mayapRollbackAvailable() ? 4U : 3U;
}

bool eventLogIsFault(const HmiEventItem &item) {
  // ACK la thao tac nguoi dung, khong chiem cho 8 ban ghi loi gan day.
  return item.type == 4U || item.type == 5U;
}

void rememberRecentFaults(const HmiEventSnapshot &snapshot, uint32_t now) {
  // Nhat ky me duoc xoa khi bat dau me moi. Khong tron loi me truoc vao me sau.
  if (snapshot.totalInWindow < lastEventWindowCount) recentFaultCount = 0U;
  for (int i = static_cast<int>(snapshot.count) - 1; i >= 0; --i) {
    const HmiEventItem &item = snapshot.items[i];
    if (static_cast<int32_t>(item.sequence - lastEventSourceSequence) <= 0 ||
        !eventLogIsFault(item)) continue;
    const uint8_t moveCount = std::min<uint8_t>(recentFaultCount,
        HMI_RECENT_FAULT_CAPACITY - 1U);
    if (moveCount) {
      memmove(&recentFaults[1], &recentFaults[0],
              moveCount * sizeof(recentFaults[0]));
      memmove(&recentFaultCapturedAt[1], &recentFaultCapturedAt[0],
              moveCount * sizeof(recentFaultCapturedAt[0]));
    }
    recentFaults[0] = item;
    recentFaultCapturedAt[0] = now;
    if (recentFaultCount < HMI_RECENT_FAULT_CAPACITY) ++recentFaultCount;
  }
  lastEventSourceSequence = snapshot.sourceSequence;
  lastEventWindowCount = snapshot.totalInWindow;
}

uint8_t eventLogVisibleCount() {
  return eventLogFaultsOnly ? recentFaultCount : currentEventLog.count;
}

const HmiEventItem *eventLogVisibleItem(uint8_t index) {
  if (eventLogFaultsOnly) return index < recentFaultCount ? &recentFaults[index] : nullptr;
  return index < currentEventLog.count ? &currentEventLog.items[index] : nullptr;
}


void stabilizeViewTransition() {
  static View lastInputView = View::Home;
  if (view == lastInputView) return;
  lastInputView = view;
  armInputGuard();
  dirty = true;
}

void executeConfirmation(bool accepted) {
  const ConfirmAction action = confirmAction;
  const View returnView = confirmReturnView;
  // Xoa trang thai xac nhan truoc khi hien thong bao o thanh day.
  confirmAction = ConfirmAction::None;

  if (accepted) {
    if (action == ConfirmAction::BatchToggle) {
      if (queueCommand(currentRuntime.batchRunning ?
                       HmiCommandType::BatchStop : HmiCommandType::BatchStart)) {
        showToast("DANG KIEM TRA DIEU KIEN");
      }
      view = View::Home;
      homePage = 0;
    } else if (action == ConfirmAction::AutoTuneStart) {
      if (queueCommand(HmiCommandType::AutoTuneStart,
                       COMMAND_AUTOTUNE_VALID_MS)) {
        showToast("DANG KHOI DONG AUTO TUNE");
      }
      view = View::AutoTune;
    } else if (action == ConfirmAction::AutoTuneCancel) {
      if (queueCommand(HmiCommandType::AutoTuneCancel,
                       COMMAND_DEFAULT_VALID_MS)) {
        showToast("DANG HUY AUTO TUNE");
      }
      view = View::AutoTune;
    } else if (action == ConfirmAction::ResumeBatch) {
      if (queueCommand(HmiCommandType::ResumeYes)) {
        resumeDecisionSubmitted = true;
        showToast("DANG PHUC HOI ME CU");
      }
      view = View::Home;
      homePage = 0;
    } else if (action == ConfirmAction::BatchOverdueContinue) {
      if (queueCommand(HmiCommandType::BatchOverdueContinue)) {
        showToast("DA XAC NHAN TIEP TUC");
      }
      batchOverdueDecisionSubmitted = true;
      view = View::Home;
      homePage = 0;
    } else if (action == ConfirmAction::TurningToggle) {
      // KHONG luu thang pendingTurningConfig - no la ban CHUP TOAN BO cau
      // hinh tu LUC MO hoi thoai, va hoi thoai nay khong co han tu dong dong
      // (nguoi dung co the can nhac vai giay toi vai phut). Neu trong luc do
      // web gui 1 thay doi khac (vd doi SV) toi applyHostConfig() cap nhat
      // currentConfig, luu thang pendingTurningConfig se GHI DE mat thay doi
      // do ma khong ai hay. Chi lay LAI dung 1 truong turningEnabled tu ban
      // chup, ap vao BAN currentConfig HIEN TAI (moi nhat) - moi thay doi
      // khac phat sinh trong luc cho deu duoc giu nguyen.
      MachineConfig candidate = currentConfig;
      candidate.turningEnabled = pendingTurningConfig.turningEnabled;
      sanitizeConfig(candidate);
      if (startConfigSave(candidate)) {
        currentConfig = candidate;
        showToast("DANG LUU...");
      }
      view = returnView;
    } else if (action == ConfirmAction::CloudPinReset) {
      if (queueCommand(HmiCommandType::CloudPinReset)) {
        showToast("DA GUI YEU CAU DAT LAI PIN");
      }
      view = returnView;
    } else if (action == ConfirmAction::FirmwareWebApply) {
      if (queueCommand(HmiCommandType::FirmwareWebApply)) {
        view = View::FirmwareProgress;
      } else {
        view = returnView;
      }
    } else if (action == ConfirmAction::FirmwareRollback) {
      // Khong dung man rieng nhu FirmwareProgress (rollback la doi con tro
      // + khoi dong lai gan nhu tuc thi, khong co qua trinh tai % nao de
      // hien) - chi can toast bao da gui, roi ESP.restart() se tu ngat
      // ngang moi thu trong vai tram ms toi.
      if (queueCommand(HmiCommandType::FirmwareRollback)) {
        showToast("DANG QUAY LAI BAN CU...");
      }
      view = returnView;
    }
  } else {
    if (action == ConfirmAction::ResumeBatch) {
      if (queueCommand(HmiCommandType::ResumeNo)) {
        resumeDecisionSubmitted = true;
        showToast("DA HUY ME CU");
      }
      view = View::Home;
      homePage = 0;
    } else if (action == ConfirmAction::BatchOverdueContinue) {
      // Chon HUY: KHONG gui lenh gi ca - coi van keu lai dinh ky (tam tat
      // duoc qua ACK), va nguoi dung van bam "Ket thuc me" binh thuong bat
      // cu luc nao neu muon dung ngay thay vi cho du 12h tu dong dung.
      batchOverdueDecisionSubmitted = true;
      view = View::Home;
      homePage = 0;
    } else if (action == ConfirmAction::FirmwareWebApply) {
      // Bam "Khong" (ke ca khi man hinh nay tu dong bat len do nhac dinh
      // ky) - hen 12h sau nhac lai, KHONG lam gi them, ve lai man hinh
      // truoc do.
      firmwareReminderLastAt = millis();
      view = returnView;
    } else {
      view = returnView;
    }
  }
  armInputGuard();
  dirty = true;
}

bool handleInlineConfirmation() {
  if (!confirmationActive() || view == View::Alarm) return false;

  if (rotary.step) {
    confirmYes = !confirmYes;
    dirty = true;
  }
  if (rotary.button == ButtonEvent::ShortPress) {
    buzzerPlayCue(BuzzerCue::Key);
    executeConfirmation(confirmYes);
  } else if (rotary.button == ButtonEvent::LongPress) {
    buzzerPlayCue(BuzzerCue::Key);
    // Phuc hoi sau mat dien bat buoc phai chon CO/KHONG, khong cho bo qua.
    if (confirmAction != ConfirmAction::ResumeBatch) {
      executeConfirmation(false);
    }
  }
  return true;
}

void handleInput() {
  const uint32_t now = millis();
  if (!timeReached(now, inputGuardUntil)) {
    resetRotaryPending(true);
    return;
  }
  // Dang o man khoi dong: nuot moi thao tac (khong dieu huong mu trong luc
  // man hinh chua hien menu), tranh vua bat may lo xoay num la da nhay vao
  // mot trang nao do ma khong biet.
  if (splashActive) {
    // Only the diagnostic screen takes input: a press means "go to Home anyway" (alarms there still show the real fault).
    if (rotary.button == ButtonEvent::ShortPress) {
      if (splashShown == MayapBoot::SplashView::Diagnostic) mayapBootRequestHome();
      else if (splashShown == MayapBoot::SplashView::StalledDiagnostic) splashOperatorHome = true;
    }
    resetRotaryPending();
    return;
  }

  // Dang tai/flash firmware (applyPhase==1): NUOT MOI THAO TAC, khong cho
  // thoat man hinh nay giua chung - giong san pham thuong mai, tranh nguoi
  // dung tuong may "treo" roi tat nguon giua luc dang ghi flash. Loi
  // (applyPhase==2) hoac thanh cong (applyPhase==3, hau nhu khong kip thay
  // vi may khoi dong lai ngay) thi cho bam bat ky nut nao de thoat.
  if (view == View::FirmwareProgress) {
    const uint8_t phase = mayapFirmwareWebStatus().applyPhase;
    if (phase == 1U) {
      resetRotaryPending();
      return;
    }
    if (rotary.button == ButtonEvent::ShortPress ||
        rotary.button == ButtonEvent::LongPress) {
      buzzerPlayCue(BuzzerCue::Key);
      view = View::FirmwareMenu;
      dirty = true;
    }
    return;
  }

  if (handleInlineConfirmation()) return;

  if (rotary.button == ButtonEvent::ShortPress ||
      rotary.button == ButtonEvent::LongPress) {
    buzzerPlayCue(BuzzerCue::Key);
  }

  if (rotary.button == ButtonEvent::LongPress) {
    // Trong workflow test nhiet, nhan giu KHONG duoc thoat tat ca va bo qua
    // 2 cau xac nhan bat buoc. Muon dung heater som thi nhan NGAN; o man
    // xac nhan phai chon CO/KHONG va nhan ngan de ghi ket qua.
    if (view == View::TestMode && heaterTestUiPhase != HeaterTestUiPhase::Idle) {
      resetRotaryPending();
      return;
    }
    if (view == View::Home) {
      activateHomeContext(true);
      return;
    }
    if (view == View::Alarm) {
      // Tu man canh bao, nhan giu de xem lai cac loi gan day.
      eventLogIndex = 0U;
      eventLogFaultsOnly = true;
      eventLogReturnView = View::Home;
      view = View::EventLog;
      dirty = true;
      return;
    }
    if (view == View::SettingList) {
      exitSettingGroup();
      return;
    }
    goBack();
    return;
  }

  switch (view) {
    case View::Home:
      if (rotary.step) {
        int p = static_cast<int>(homePage) + rotary.step;
        if (p < 0) p = 1;
        if (p > 1) p = 0;
        homePage = static_cast<uint8_t>(p);
        dirty = true;
      }
      if (rotary.button == ButtonEvent::ShortPress) {
        activateHomeContext(false);
      }
      break;

    case View::MainMenu:
      if (rotary.step) {
        mainIndex = static_cast<uint8_t>(constrain(static_cast<int>(mainIndex) + rotary.step, 0, MAIN_COUNT - 1));
        if (mainIndex < listTop) listTop = mainIndex;
        if (mainIndex >= listTop + 4) listTop = mainIndex - 3;
        dirty = true;
      }
      if (rotary.button == ButtonEvent::ShortPress) selectMainItem();
      break;

    case View::SettingList: {
      const uint8_t itemCount = settingListItemCount(selectedGroup);
      if (rotary.step) {
        setListSelection(static_cast<int>(listIndex) + rotary.step, itemCount);
      }
      if (rotary.button == ButtonEvent::ShortPress) {
        const SettingGroup &group = GROUPS[selectedGroup];
        const uint8_t extraCount = groupVisibleExtraCount(selectedGroup);
        if (listIndex < group.count) {
          openSetting();
        } else if (listIndex < group.count + extraCount) {
          const GroupExtra extra = visibleGroupExtraAt(selectedGroup,
              static_cast<uint8_t>(listIndex - group.count));
          if (extra == GroupExtra::AutoTuneEntry) {
            openAutoTuneEntry();
          } else if (extra == GroupExtra::TurnStats) {
            view = View::TurnStats;
            dirty = true;
          } else if (extra == GroupExtra::WifiChange) {
            openWifiChange();
          } else if (extra == GroupExtra::ConnectionInfo) {
            view = View::ConnectionInfo;
            dirty = true;
          } else if (extra == GroupExtra::QrCode) {
            view = View::QrCode;
            dirty = true;
          } else if (extra == GroupExtra::CloudPinReset) {
            openCloudPinResetConfirm();
          } else if (extra == GroupExtra::FirmwareWebUpdate) {
            firmwareMenuIndex = 0U;
            view = View::FirmwareMenu;
            dirty = true;
          } else if (extra == GroupExtra::VentilationEntry) {
            ventilationIndex = 0U;
            view = View::VentilationMenu;
            dirty = true;
          }
        } else {
          exitSettingGroup();
        }
      }
      break;
    }

    case View::ChungMenu:
      if (rotary.step) {
        chungIndex = static_cast<uint8_t>(constrain(
            static_cast<int>(chungIndex) + rotary.step, 0, chungItemCount() - 1U));
        if (chungIndex < chungTop) chungTop = chungIndex;
        if (chungIndex >= chungTop + 4) chungTop = chungIndex - 3;
        dirty = true;
      }
      if (rotary.button == ButtonEvent::ShortPress) selectChungItem();
      break;

    case View::VentilationMenu: {
      const uint8_t count = ventilationMenuCount();
      if (ventilationIndex >= count) ventilationIndex = 0U;
      if (rotary.step) {
        ventilationIndex = static_cast<uint8_t>(constrain(
            static_cast<int>(ventilationIndex) + rotary.step, 0, count - 1));
        dirty = true;
      }
      if (rotary.button == ButtonEvent::ShortPress) {
        if (ventilationIndex == 0U) openSettingIndex(42U, View::VentilationMenu);
        else if (currentConfig.ventAutoEnabled && ventilationIndex == 1U)
          openSettingIndex(43U, View::VentilationMenu);
        else if (currentConfig.ventAutoEnabled && ventilationIndex == 2U) {
          ventilationAdvancedIndex = 0U;
          view = View::VentilationAdvanced;
          dirty = true;
        } else goBack();
      }
      break;
    }
    case View::VentilationAdvanced:
      if (rotary.step) {
        ventilationAdvancedIndex = static_cast<uint8_t>(constrain(
            static_cast<int>(ventilationAdvancedIndex) + rotary.step, 0, 7));
        dirty = true;
      }
      if (rotary.button == ButtonEvent::ShortPress) {
        if (ventilationAdvancedIndex < 7U)
          openSettingIndex(static_cast<uint8_t>(44U + ventilationAdvancedIndex),
                           View::VentilationAdvanced);
        else goBack();
      }
      break;
    case View::FirmwareMenu: {
      const uint8_t count = firmwareMenuCount();
      if (firmwareMenuIndex >= count) firmwareMenuIndex = count - 1U;
      if (rotary.step) {
        firmwareMenuIndex = static_cast<uint8_t>(constrain(
            static_cast<int>(firmwareMenuIndex) + rotary.step, 0, count - 1));
        dirty = true;
      }
      if (rotary.button == ButtonEvent::ShortPress) {
        if (firmwareMenuIndex == 0U) {
          char version[25];
          snprintf(version, sizeof(version), "HIEN TAI v%s", MAYAP_FIRMWARE_VERSION);
          showToast(version);
        } else if (firmwareMenuIndex == 1U) openFirmwareWebConfirm();
        else if (mayapRollbackAvailable() && firmwareMenuIndex == 2U)
          openFirmwareRollbackConfirm();
        else goBack();
      }
      break;
    }

    case View::EditSetting: {
      const SettingItem &item = SETTINGS[editSettingIndex];
      if (rotary.step) {
        float minimum, maximum;
        settingLimits(currentConfig, item, minimum, maximum);
        editValue = constrain(editValue + rotary.step * item.step, minimum, maximum);
        dirty = true;
      }
      if (rotary.button == ButtonEvent::ShortPress) commitSetting();
      break;
    }

    case View::TurnStats:
      if (rotary.button == ButtonEvent::ShortPress) goBack();
      break;

    case View::ConnectionInfo:
      if (rotary.button == ButtonEvent::ShortPress) goBack();
      break;

    case View::QrCode:
      if (rotary.button == ButtonEvent::ShortPress) goBack();
      break;

    case View::AutoTune:
      if (rotary.button == ButtonEvent::ShortPress) openAutoTuneConfirm();
      break;

    case View::TestMode: {
      if (heaterTestUiPhase == HeaterTestUiPhase::Heating) {
        if (rotary.button == ButtonEvent::ShortPress) finishHeaterPowerPhase(now);
        break;
      }
      if (heaterTestUiPhase == HeaterTestUiPhase::ConfirmHeat) {
        if (rotary.step) { heaterTestConfirmYes = !heaterTestConfirmYes; dirty = true; }
        if (rotary.button == ButtonEvent::ShortPress) {
          testDeviceResult[static_cast<uint8_t>(TestOutputId::HeaterSsr)] =
              heaterTestConfirmYes ? TestResult::Pass : TestResult::Fail;
          if (queueCommand(HmiCommandType::TestOutputPulse, COMMAND_DEFAULT_VALID_MS,
                           0, static_cast<uint32_t>(TestOutputId::VentFan))) {
            testModeLastCommandAt = now;
            heaterTestUiPhase = HeaterTestUiPhase::ConfirmVent;
            heaterTestConfirmYes = true;
            armInputGuard();
          } else {
            testDeviceResult[static_cast<uint8_t>(TestOutputId::VentFan)] = TestResult::Fail;
            heaterTestUiPhase = HeaterTestUiPhase::Idle;
          }
          dirty = true;
        }
        break;
      }
      if (heaterTestUiPhase == HeaterTestUiPhase::ConfirmVent) {
        if (rotary.step) { heaterTestConfirmYes = !heaterTestConfirmYes; dirty = true; }
        if (rotary.button == ButtonEvent::ShortPress) {
          testDeviceResult[static_cast<uint8_t>(TestOutputId::VentFan)] =
              heaterTestConfirmYes ? TestResult::Pass : TestResult::Fail;
          (void)queueCommand(HmiCommandType::TestOutputStop, COMMAND_DEFAULT_VALID_MS,
                             0, static_cast<uint32_t>(TestOutputId::VentFan));
          testModeLastCommandAt = now;
          heaterTestUiPhase = HeaterTestUiPhase::Idle;
          dirty = true;
        }
        break;
      }

      // Cac output con lai van dung hoi CO/KHONG chung nhu cu.
      if (testDeviceConfirmActive) {
        if (rotary.step) {
          testDeviceConfirmYes = !testDeviceConfirmYes;
          dirty = true;
        }
        if (rotary.button == ButtonEvent::ShortPress) {
          testDeviceResult[testDeviceConfirmIndex] =
              testDeviceConfirmYes ? TestResult::Pass : TestResult::Fail;
          queueCommand(HmiCommandType::TestOutputStop, COMMAND_DEFAULT_VALID_MS,
                      0, static_cast<uint32_t>(testDeviceConfirmIndex));
          testModeLastCommandAt = now;
          testDeviceConfirmActive = false;
          dirty = true;
        }
        break;
      }
      if (rotary.step) {
        setListSelection(static_cast<int>(listIndex) + rotary.step,
                         TEST_MODE_ITEM_COUNT);
      }
      if (rotary.button == ButtonEvent::ShortPress) {
        if (listIndex < TEST_MODE_OUTPUT_ROWS) {
          if (queueCommand(HmiCommandType::TestOutputPulse,
                           COMMAND_DEFAULT_VALID_MS, 0,
                           static_cast<uint32_t>(listIndex))) {
            testModeLastCommandAt = now;
            if (listIndex == static_cast<uint8_t>(TestOutputId::HeaterSsr)) {
              heaterTestUiPhase = HeaterTestUiPhase::Heating;
              heaterTestUiStartedAt = now;
              heaterTestUiLastRefreshAt = now;
              heaterTestConfirmYes = true;
              testDeviceResult[static_cast<uint8_t>(TestOutputId::HeaterSsr)] = TestResult::Untested;
              testDeviceResult[static_cast<uint8_t>(TestOutputId::VentFan)] = TestResult::Untested;
            } else {
              testDeviceConfirmActive = true;
              testDeviceConfirmIndex = listIndex;
              testDeviceConfirmYes = true;
            }
            armInputGuard();
          }
        } else if (listIndex < TEST_MODE_OUTPUT_ROWS + TEST_MODE_LIMIT_ROWS) {
          testLimitSelected = (listIndex == TEST_MODE_OUTPUT_ROWS)
              ? TestLimitId::Left : TestLimitId::Right;
          queueCommand(HmiCommandType::TestLimitStart, COMMAND_DEFAULT_VALID_MS,
                      0, static_cast<uint32_t>(testLimitSelected));
          testModeLastCommandAt = now;
        } else {
          testSummaryIndex = 0;
          view = View::TestSummary;
        }
        dirty = true;
      }
      break;
    }

    case View::TestSummary:
      if (rotary.step) {
        testSummaryIndex = static_cast<uint8_t>(constrain(
            static_cast<int>(testSummaryIndex) + rotary.step, 0,
            TEST_SUMMARY_ITEM_COUNT - 1));
        dirty = true;
      }
      if (rotary.button == ButtonEvent::ShortPress) goBack();
      break;

    case View::WifiChange:
      if (rotary.button == ButtonEvent::ShortPress) goBack();
      break;

    case View::EventLog: {
      const uint8_t count = eventLogVisibleCount();
      if (count && rotary.step) {
        int next = static_cast<int>(eventLogIndex) + rotary.step;
        while (next < 0) next += count;
        while (next >= count) next -= count;
        eventLogIndex = static_cast<uint8_t>(next);
        dirty = true;
      }
      if (rotary.button == ButtonEvent::ShortPress) goBack();
      break;
    }

    case View::Alarm: {
      const uint8_t count = currentRuntime.activeFaultDisplayCount;
      if (!count) {
        view = alarmReturnView;
        dirty = true;
        break;
      }
      if (rotary.step) {
        int next = static_cast<int>(alarmIndex) + rotary.step;
        while (next < 0) next += count;
        while (next >= count) next -= count;
        alarmIndex = static_cast<uint8_t>(next);
        dirty = true;
      }
      if (rotary.button == ButtonEvent::ShortPress) {
        // Nhan ngan chi ACK/tat coi va quay lai man dang dung. Loi van con
        // trong he thong, co the xem lai bang nhan giu tai Home.
        if (requestAlarmAcknowledge()) {
          view = alarmReturnView;
          dirty = true;
        }
      }
      break;
    }

    case View::TurnStatus:
      if (rotary.button == ButtonEvent::ShortPress) goBack();
      break;
  }
}

// ============================================================
// 6. VE GIAO DIEN
// ============================================================
// showDate=false cho cac man hinh trong luong "cai dat" (CAI DAT CHUNG va
// cac man con cua no) - ngay thang khong can thiet khi dang chinh thong so,
// bot roi mat va nhuong cho tieu de dai hon (xem drawEditSetting()).
void drawHeader(const char *title, bool showDate) {
  lcd.setDrawColor(1);
  lcd.setFont(u8g2_font_5x8_tf);
  char shortTitle[14];
  snprintf(shortTitle, sizeof(shortTitle), "%.13s", title ? title : "");
  lcd.drawStr(0, 8, shortTitle);
  const char *date = currentRuntime.dateText[0] ? currentRuntime.dateText : "--/--/----";
  const int16_t dateX = max(0, 127 - static_cast<int16_t>(lcd.getStrWidth(date)));
  if (currentRuntime.alarmMask) {
    lcd.drawBox(67, 0, 8, 8);
    lcd.setDrawColor(0);
    lcd.drawStr(69, 8, "!");
    lcd.setDrawColor(1);
  }
  if (showDate) lcd.drawStr(dateX, 8, date);
  lcd.drawHLine(0, 10, 128);
}
void drawHeader(const char *title) { drawHeader(title, true); }

void drawListPosition(uint8_t index, uint8_t count) {
  if (!count) return;
  char position[8];
  snprintf(position, sizeof(position), "%u/%u", index + 1U, count);
  lcd.setFont(u8g2_font_5x8_tf);
  lcd.drawStr(max(78, 127 - static_cast<int16_t>(lcd.getStrWidth(position))),
              8, position);
}

// ---------------------- Chong tran chu dung chung ----------------------
// Man hinh chi rong 128px. Moi cho ve chuoi co do dai THAY DOI theo du lieu
// (ten loi, ten Wi-Fi, ten thiet bi, noi dung su kien...) deu phai di qua
// day thay vi drawStr() thang, neu khong se bi cat mat chu o bien man hinh
// ma khong bao gi - da tung xay ra o nhieu man khac nhau.
//
// Chon font LON NHAT trong 3 muc ma chuoi van vua maxWidth, va dat font do
// lam font hien hanh. Tra ve be rong that de goi ben ngoai tu dat vi tri.
int16_t fitFontWidth(const char *text, int16_t maxWidth,
                     const uint8_t *big, const uint8_t *mid,
                     const uint8_t *small) {
  if (!text) text = "";
  lcd.setFont(big);
  int16_t w = static_cast<int16_t>(lcd.getStrWidth(text));
  if (w <= maxWidth) return w;
  lcd.setFont(mid);
  w = static_cast<int16_t>(lcd.getStrWidth(text));
  if (w <= maxWidth) return w;
  lcd.setFont(small);
  return static_cast<int16_t>(lcd.getStrWidth(text));
}

// Ve can giua, tu ha co font neu qua dai. maxWidth chua le 2px moi ben.
void drawCenteredFit(int16_t y, const char *text, const uint8_t *big,
                     const uint8_t *mid, const uint8_t *small,
                     int16_t maxWidth = 124) {
  if (!text) text = "";
  const int16_t w = fitFontWidth(text, maxWidth, big, mid, small);
  lcd.drawStr(max(0, (128 - w) / 2), y, text);
}

// Ve can trai tai x, tu ha co font neu qua dai so voi phan man hinh con lai.
void drawLeftFit(int16_t x, int16_t y, const char *text, const uint8_t *big,
                 const uint8_t *mid, const uint8_t *small) {
  if (!text) text = "";
  fitFontWidth(text, static_cast<int16_t>(126 - x), big, mid, small);
  lcd.drawStr(x, y, text);
}

// Nhu drawLeftFit nhung chi dinh RO be rong toi da cho phep (dung khi ben
// phai da co noi dung khac chiem cho, vd cot gia tri trong danh sach cai dat).
void drawLeftFit2(int16_t x, int16_t y, const char *text, int16_t maxRight,
                  const uint8_t *big, const uint8_t *small) {
  if (!text) text = "";
  fitFontWidth(text, static_cast<int16_t>(maxRight - x), big, small, small);
  lcd.drawStr(x, y, text);
}

uint32_t alarmBitForFaultCode(uint16_t code) {
  switch (code) {
    case 101: case 102: case 103: case 104: return AlarmSensor;
    case 110: return AlarmTempLow;
    case 111: return AlarmTempHigh;
    case 112: return AlarmEmergency;
    case 113: case 114: return AlarmTempHigh;
    case 115: return AlarmSystem;
    case 120: return AlarmHumidityLow;
    case 121: return AlarmHumidityLow;
    case 130: case 131: case 132: return AlarmSystem;
    case 133: return AlarmAutoMode;
    case 134: return AlarmTurning;
    case 135: case 136: case 137: return AlarmSystem;
    case 201: case 202: case 203: case 204: case 205: return AlarmTurning;
    case 301: case 302: case 303: case 304: case 305: case 306:
    case 313: case 314: case 315: return AlarmSystem;
    case 501: case 502: return AlarmSystem;
    default: return AlarmSystem;
  }
}

const char *faultTitle(uint16_t code) {
  switch (code) {
    case 101: return "MAT CAM BIEN";
    case 102: return "CAM BIEN SAI";
    case 103: return "CAM BIEN BAT THUONG";
    case 104: return "CAM BIEN DUNG HINH";
    case 110: return "NHIET DO THAP";
    case 111: return "NHIET DO CAO";
    case 112: return "QUA NHIET KHAN";
    case 113: return "NHIET DO BIEN THIEN NHANH";
    case 114: return "NHIET DO KHONG ON DINH";
    case 115: return "THANH NHIET KHONG NONG";
    case 120: return "DO AM THAP";
    case 121: return "DO AM CAO";
    case 130: return "TAT CONG TAC NHIET";
    case 131: return "QUAT TUAN HOAN OFF";
    case 132: return "CAN CHUYEN SANG AUTO";
    case 133: return "AUTO BI TAT GIUA ME";
    case 134: return "TU DONG DAO BI TAT";
    case 135: return "CHO XAC NHAN AP LAI";
    case 136: return "ME QUA HAN AP";
    case 137: return "CHO RTC QUA LAU";
    case 201: return "LOI 2 HANH TRINH";
    case 202: return "DAO QUA THOI GIAN";
    case 203: return "HANH TRINH BI KET";
    case 204: return "XUNG DOT LENH DAO";
    case 205: return "CAN KIEM TRA CO KHI DAO";
    case 301: return "MAT EEPROM";
    case 302: return "EEPROM SUY GIAM";
    case 303: return "RESET BAT THUONG";
    case 304: return "XUNG DOT OUTPUT";
    case 305: return "RELAY QUA NHIEU";
    case 306: return "LOI RTC";
    case 313: return "CHUA XOA DU LIEU ME";
    case 314: return "LOI NHAT KY AN TOAN";
    case 315: return "MAT NHAT KY ME";
    case 401: return "BO NHO RAM THAP";
    case 402: return "RAM SAP CAN";
    case 403: return "XU HUONG NHIET BAT THUONG";
    case 404: return "EEPROM GHI LOI NHIEU";
    case 501: return "MAT LIEN LAC ATTINY";
    case 502: return "PIN COI SAP HET";
    case 503: return "LECH TRANG THAI ATTINY";
    default: return "LOI KHONG XAC DINH";
  }
}

void faultDetail(const HmiFaultItem &fault, char *out, size_t size) {
  switch (fault.code) {
    case 101: snprintf(out, size, "KHONG CO DU LIEU RS485"); break;
    case 102: snprintf(out, size, "DU LIEU NGOAI PHAM VI"); break;
    case 103: snprintf(out, size, "MAU NGHI NGO %.1fC", fault.detail * 0.1f); break;
    case 104: snprintf(out, size, "PV KET %.1fC - DA CAT NHIET", fault.detail * 0.1f); break;
    case 110: snprintf(out, size, "PV %.1f < %.1fC", currentRuntime.temperature,
                       currentConfig.lowTempAlarm); break;
    case 111: snprintf(out, size, "PV %.1f > %.1fC", fault.detail * 0.1f,
                       currentConfig.highTempAlarm); break;
    case 112: snprintf(out, size, "PV %.1f > %.1fC", fault.detail * 0.1f,
                       currentConfig.emergencyTemp); break;
    case 113: snprintf(out, size, "PV %.1fC LECH NHANH", fault.detail * 0.1f); break;
    case 114: snprintf(out, size, "%d LAN DOI CHIEU/10P", fault.detail); break;
    case 115: snprintf(out, size, "TANG %.1fC/15P (THAP)", fault.detail * 0.1f); break;
    case 120: snprintf(out, size, "AM %.0f%% < %.0f%%", currentRuntime.humidity,
                       currentConfig.lowHumidityAlarm); break;
    case 121: snprintf(out, size, "AM %.0f%% > %.0f%%", currentRuntime.humidity,
                       HUMIDITY_HIGH_ALARM_C); break;
    case 130: snprintf(out, size, "NHIET DA BI KHOA"); break;
    case 131: snprintf(out, size, "MANUAL: HAY BAT QUAT"); break;
    case 132: snprintf(out, size, "PHUC HOI ME DANG CHO"); break;
    case 133: snprintf(out, size, "HAY BAT LAI AUTO"); break;
    case 134: snprintf(out, size, "DAO TU DONG PHAI LUON ON"); break;
    case 135: snprintf(out, size, "%d PHUT CHUA XAC NHAN", fault.detail); break;
    case 136: snprintf(out, size, "QUA %d NGAY", fault.detail); break;
    case 137: snprintf(out, size, "%d PHUT CHUA RO RTC", fault.detail); break;
    case 201: snprintf(out, size, "HAI CTHT CUNG TAC DONG"); break;
    case 202: snprintf(out, size, "CHUA CHAM CTHT DICH"); break;
    case 203: snprintf(out, size, "CTHT GOC KHONG NHA"); break;
    case 204: snprintf(out, size, "TRAI VA PHAI CUNG ON"); break;
    case 205: snprintf(out, size, "%d LAN LOI - VAO TEST MODE", fault.detail); break;
    case 301: snprintf(out, size, "KHONG DOC/GHI DUOC"); break;
    case 302: snprintf(out, size, "DANG CHAY BANG RAM"); break;
    case 303: snprintf(out, size, "KIEM TRA RESET/WDT"); break;
    case 304: snprintf(out, size, "DA CAT OUTPUT AN TOAN"); break;
    case 305: snprintf(out, size, "%d LAN DONG CAT/GIO", fault.detail); break;
    case 306:
      if (fault.detail == 1) snprintf(out, size, "RTC OSF/THOI GIAN SAI");
      else if (fault.detail == 2) snprintf(out, size, "MAT KET NOI RTC 0x68");
      else snprintf(out, size, "RTC KHONG TANG GIAY");
      break;
    case 313: snprintf(out, size, "DANG THU LAI EEPROM"); break;
    case 314: snprintf(out, size, "NVS NOI BO KHONG SAN SANG"); break;
    case 315: snprintf(out, size, "FLASH LOG KHONG GHI DUOC"); break;
    case 401: case 402:
      snprintf(out, size, "RAM CON %d%%", fault.detail); break;
    case 403:
      snprintf(out, size, "%s %d PHUT TOI NGUONG",
               fault.detail < 0 ? "GIAM" : "TANG", abs(fault.detail)); break;
    case 404:
      snprintf(out, size, "EEPROM THU LAI %d LAN", fault.detail); break;
    case 501: snprintf(out, size, "ATTINY KHONG PHAN HOI"); break;
    case 502: snprintf(out, size, "HAY THAY PIN 9V SOM"); break;
    case 503: snprintf(out, size, "KIEM TRA DONG BO TRANG THAI"); break;
    default: snprintf(out, size, "CHI TIET %d", fault.detail); break;
  }
}

void drawAlarm() {
  const uint8_t count = currentRuntime.activeFaultDisplayCount;
  if (!count) {
    drawHeader("CANH BAO");
    lcd.setFont(u8g2_font_6x12_tf);
    lcd.drawStr(22, 36, "KHONG CO CANH BAO");
    return;
  }
  if (alarmIndex >= count) alarmIndex = 0;
  const HmiFaultItem &fault = currentRuntime.activeFaults[alarmIndex];
  char detail[30];
  char footer[40];
  drawHeader("CANH BAO");

  // Tu co font neu tieu de qua dai cho 128px (mot vai ten loi - ke ca vai ma
  // co tu truoc, khong chi ma moi them - dai hon du de tran man o co dinh
  // helvB10 truoc day, gay mat chu). Thu tu uu tien: dam/lon truoc, nho dan.
  drawCenteredFit(29, faultTitle(fault.code), u8g2_font_helvB10_tf,
                  u8g2_font_6x12_tf, u8g2_font_5x8_tf);

  faultDetail(fault, detail, sizeof(detail));
  drawCenteredFit(40, detail, u8g2_font_5x8_tf, u8g2_font_5x8_tf,
                  u8g2_font_5x8_tf);
  snprintf(footer, sizeof(footer), "E%03u  %u/%u  XOAY:LOI",
           fault.code, alarmIndex + 1U, count);
  lcd.setFont(u8g2_font_5x8_tf);
  lcd.drawStr(1, 51, footer);
  lcd.drawStr(1, 61, "NHAN:TAT COI  GIU:NHAT KY");
}

void drawCenteredText(int16_t y, const char *text) {
  if (!text) text = "";
  const int16_t width = static_cast<int16_t>(lcd.getStrWidth(text));
  lcd.drawStr(max(0, (128 - width) / 2), y, text);
}

// Minimal splash requested by the operator: centered logo and three dots.
// Boot diagnostic (replaces the logo when the Home gate is still closed after 30 s): the concrete code and cause, never "ready".
void drawBootDiagnostic(MayapBoot::BlockReason why, uint16_t codeNumber, uint32_t ageMs) {
  const char *line1 = "KIEM TRA THIET BI";
  const char *line2 = "";
  switch (why) {
    case MayapBoot::BlockReason::SensorNoResponse: line1 = "CAM BIEN KHONG TRA LOI"; line2 = "KIEM TRA DAY RS485 / NGUON"; break;
    case MayapBoot::BlockReason::SensorFormat: line1 = "CAM BIEN SAI DINH DANG"; line2 = "KIEM TRA LOAI CAM BIEN"; break;
    case MayapBoot::BlockReason::SensorUnstable: line1 = "CAM BIEN CHUA ON DINH"; line2 = "KIEM TRA DAY / NHIEU"; break;
    case MayapBoot::BlockReason::Storage: line1 = "BO NHO EEPROM LOI"; line2 = "KIEM TRA CHIP EEPROM"; break;
    case MayapBoot::BlockReason::Tasks: line1 = "LOI TIEN TRINH HE THONG"; line2 = "TAT/BAT NGUON LAI"; break;
    case MayapBoot::BlockReason::SafetyTrip: line1 = "NGAT AN TOAN HE THONG"; line2 = "TAT/BAT NGUON LAI"; break;
    case MayapBoot::BlockReason::Display: line1 = "LOI MAN HINH LCD"; line2 = "KIEM TRA DAY I2C"; break;
    case MayapBoot::BlockReason::Coordinator: line1 = "KHOI DONG KHONG PHAN HOI"; line2 = "TAT/BAT NGUON LAI"; break;
    default: break;
  }
  char code[28];
  snprintf(code, sizeof(code), "E%u %s", static_cast<unsigned>(codeNumber), MayapBoot::blockText(why));
  lcd.setDrawColor(1);
  lcd.setFont(u8g2_font_5x8_tf);
  char head[28];
  snprintf(head, sizeof(head), "CHAN DOAN KHOI DONG %lus", static_cast<unsigned long>(ageMs / 1000UL));
  lcd.drawStr(0, 8, head);
  lcd.drawHLine(0, 10, 128);
  drawCenteredFit(24, code, u8g2_font_helvB10_tf, u8g2_font_6x12_tf, u8g2_font_5x8_tf);
  drawCenteredFit(36, line1, u8g2_font_6x12_tf, u8g2_font_5x8_tf, u8g2_font_5x8_tf);
  drawCenteredFit(46, line2, u8g2_font_5x8_tf, u8g2_font_5x8_tf, u8g2_font_5x8_tf);
  drawCenteredFit(56, "DIEU KHIEN CUC BO VAN CHAY", u8g2_font_5x8_tf, u8g2_font_5x8_tf, u8g2_font_5x8_tf);
  drawCenteredFit(63, "NHAN NUT: VAO MAN HINH CHINH", u8g2_font_5x8_tf, u8g2_font_5x8_tf, u8g2_font_5x8_tf);
}

void drawSplash() {
  if (splashShown == MayapBoot::SplashView::StalledDiagnostic) {
    drawBootDiagnostic(MayapBoot::BlockReason::Coordinator, MayapBoot::blockCode(MayapBoot::BlockReason::Coordinator), millis());
    return;
  }
  if (splashShown == MayapBoot::SplashView::Diagnostic) {
    drawBootDiagnostic(mayapBootDiagnosticReason(), mayapBootDiagnosticCode(), mayapBootDiagnosticAgeMs());
    return;
  }
  lcd.setDrawColor(1);
  lcd.drawXBMP((128 - BOOT_LOGO_WIDTH) / 2, BOOT_LOGO_TOP,
               BOOT_LOGO_WIDTH, BOOT_LOGO_HEIGHT, bootLogoBits);
  lcd.drawDisc(60, 57, 1);
  lcd.drawDisc(64, 57, 1);
  lcd.drawDisc(68, 57, 1);
}

void drawHomeMain() {
  char text[32];
  // Bo chu "MAY AP" (khong mang thong tin gi them), thay bang gio:phut
  // ngay tai vi tri tieu de - truoc day gio bi nhet vao 1 khe rat hep
  // [36,64] giua tieu de va ngay thang, gio co ca vi tri tieu de rong rai.
  drawHeader(currentRuntime.timeText);

  // Man tong quan: PV la thong tin uu tien so 1. Chia man hinh thanh
  // vung PV ben trai (~70 px) va 3 dong thong tin phu ben phai.
  // Khong dung khung/duong ke de tiet kiem diem anh va giu giao dien thoang.
  constexpr int16_t TEMP_ZONE_WIDTH = 70;
  // Dich cot SV/AM/DAO ra gan mep phai hon de khong bi dinh sat vung nhiet do,
  // giup hai ben man hinh can doi hon. 76 (khong phai 80): cac dong dai nhat
  // ("SV 37.5C", "DAO 720p", "DAO KHOA" - 8 ky tu font 6x12 = 48px) o x=80 se
  // ket thuc dung diem anh cuoi cung 127, dinh sat mep man hinh; 76 chua lai
  // le phai 4px cho can doi va van cach vung nhiet do (rong 70) 6px.
  constexpr int16_t RIGHT_X = 76;

  snprintf(text, sizeof(text), currentRuntime.sensorOnline ? "%.1f" : "--.-",
           currentRuntime.temperature);
  // Uu tien font 26 px; neu gia tri bat thuong dai hon vung PV thi tu dong
  // ha font de khong cat mat ky tu o bien man hinh.
  lcd.setFont(u8g2_font_logisoso26_tn);
  int16_t tempWidth = static_cast<int16_t>(lcd.getStrWidth(text));
  if (tempWidth > TEMP_ZONE_WIDTH) {
    lcd.setFont(u8g2_font_logisoso24_tn);
    tempWidth = static_cast<int16_t>(lcd.getStrWidth(text));
  }
  if (tempWidth > TEMP_ZONE_WIDTH) {
    lcd.setFont(u8g2_font_logisoso22_tn);
    tempWidth = static_cast<int16_t>(lcd.getStrWidth(text));
  }
  const int16_t tempX = max(0, (TEMP_ZONE_WIDTH - tempWidth) / 2);
  lcd.drawStr(tempX, 40, text);

  // Ba mang thong tin phu duoc dan deu theo chieu doc: SV / AM / DAO TIEP.
  lcd.setFont(u8g2_font_6x12_tf);
  snprintf(text, sizeof(text), "SV %.1fC", currentConfig.targetTemp);
  lcd.drawStr(RIGHT_X, 20, text);
  if (currentRuntime.sensorOnline) {
    snprintf(text, sizeof(text), currentRuntime.humidifierOn ? "AM %.0f%%+" : "AM %.0f%%",
             currentRuntime.humidity);
  } else {
    snprintf(text, sizeof(text), "AM --%%");
  }
  lcd.drawStr(RIGHT_X, 34, text);
  if (currentRuntime.turningLockdown) {
    snprintf(text, sizeof(text), "DAO KHOA");
  } else if (currentRuntime.nextTurnScheduled) {
    snprintf(text, sizeof(text), "DAO %up", currentRuntime.nextTurnMinutes);
  } else {
    snprintf(text, sizeof(text), "DAO --");
  }
  lcd.drawStr(RIGHT_X, 48, text);

  // Ngay ap nam rieng duoi vung PV. Trang thai may nam mot dong rieng,
  // can giua o day man hinh de khong bi "dinh" vao thong tin ngay ap.
  lcd.setFont(u8g2_font_5x8_tf);
  snprintf(text, sizeof(text), "NGAY %u/%u", currentRuntime.currentDay,
           currentConfig.totalIncubationDays);
  const int16_t dayWidth = static_cast<int16_t>(lcd.getStrWidth(text));
  lcd.drawStr(max(0, (TEMP_ZONE_WIDTH - dayWidth) / 2), 52, text);

  if (currentRuntime.activeFaultCount) {
    snprintf(text, sizeof(text), "!E%03u %.18s",
             currentRuntime.primaryFaultCode, currentRuntime.machineState);
  } else if (onlineServiceDegradedMaskSeen) {
    snprintf(text, sizeof(text), "ONLINE LOI - MAY VAN CHAY");
  } else if (currentRuntime.connectivityMode == ConnectivityMode::Online &&
             currentRuntime.networkConfigured &&
             !currentRuntime.networkConnected &&
             currentRuntime.wifiPortalState == WifiPortalState::Idle) {
    snprintf(text, sizeof(text), "MAT WIFI - MAY VAN CHAY");
  } else if (currentRuntime.connectivityMode == ConnectivityMode::Online &&
             currentRuntime.networkConnected &&
             currentRuntime.networkRssiDbm <= WIFI_RSSI_WEAK_DBM) {
    snprintf(text, sizeof(text), "WIFI YEU - MAY VAN CHAY");
  } else {
    snprintf(text, sizeof(text), "%.25s", currentRuntime.machineState);
  }
  drawCenteredText(63, text);
}

const char *turnStateShort(TurnState state) {
  switch (state) {
    case TurnState::Left: return "TRAI";
    case TurnState::Right: return "PHAI";
    case TurnState::Waiting: return "CHO";
    case TurnState::Fault: return "LOI";
    default: return "DUNG";
  }
}

// Man phu tu dong bat khi dong co dang chay dao trung hoac tim goc - xem co
// che tu bat/tu tat tai applyRuntime() (giong het man Alarm). Chi 1 trong 2
// truong hop hien tai duoc ve (turnHoming uu tien vi noi ro dong co dang lam
// gi hon "dao trung" don thuan khi vi tri khay con chua xac dinh).
void drawTurnStatus() {
  char line[24];
  if (currentRuntime.turnHoming) {
    drawHeader("TIM GOC");
    drawCenteredFit(29, "DANG TIM GOC", u8g2_font_helvB10_tf,
                    u8g2_font_6x12_tf, u8g2_font_5x8_tf);
    lcd.setFont(u8g2_font_5x8_tf);
    drawCenteredText(50, "DINH VI LAI KHAY TRUNG");
  } else {
    drawHeader("DAO TRUNG");
    drawCenteredFit(29, "DANG DAO TRUNG", u8g2_font_helvB10_tf,
                    u8g2_font_6x12_tf, u8g2_font_5x8_tf);
    lcd.setFont(u8g2_font_6x12_tf);
    snprintf(line, sizeof(line), "HUONG: %s",
             turnStateShort(currentRuntime.turnState));
    drawCenteredText(50, line);
  }
}

void drawStatusPair(int16_t y, const char *left, const char *right) {
  lcd.drawStr(1, y, left ? left : "");
  if (!right) return;
  const int16_t rightWidth = static_cast<int16_t>(lcd.getStrWidth(right));
  lcd.drawStr(max(1, 127 - rightWidth), y, right);
}

// Cot phai co vi tri X co dinh (khong phu thuoc do dai "ON"/"OFF"), de OUT
// va HUT luon thang cot voi nhau giua cac lan cap nhat.
constexpr int16_t STATUS_RIGHT_COLUMN_X = 68;
void drawStatusPairFixed(int16_t y, const char *left, const char *right) {
  lcd.drawStr(1, y, left ? left : "");
  if (right) lcd.drawStr(STATUS_RIGHT_COLUMN_X, y, right);
}

void drawHomeOutputs() {
  char left[24];
  char right[24];
  char turnLine[28];
  drawHeader("TRANG THAI");

  // Rut gon con 2 hang thiet bi (SSR/OUT, QUAT/HUT) + 1 dong DAO TRUNG lam
  // trong tam duoi day, trinh bay nhu mot chi bao trang thai giong man chinh.
  lcd.setFont(u8g2_font_6x12_tf);
  snprintf(left, sizeof(left), "SSR %3.0f%%", currentRuntime.heaterPower);
  snprintf(right, sizeof(right), "OUT %s", currentRuntime.heaterOn ? "ON" : "OFF");
  drawStatusPairFixed(24, left, right);

  snprintf(left, sizeof(left), "QUAT %s", currentRuntime.circulationFanOn ? "ON" : "OFF");
  snprintf(right, sizeof(right), "HUT %s", currentRuntime.ventFanOn ? "ON" : "OFF");
  drawStatusPairFixed(38, left, right);

  // Duong ngan chi bang nua chieu rong man hinh, can giua, thay cho thanh
  // ngang day duoi truoc day.
  lcd.drawHLine(32, 44, 64);

  if (currentRuntime.turningLockdown) {
    snprintf(turnLine, sizeof(turnLine), "DAO TRUNG: KHOA");
  } else if (currentRuntime.turnHoming) {
    // Dong co dang chay tim goc (chua ro vi tri khay), KHAC voi dao dinh ky
    // binh thuong - phai bao rieng de nguoi dung khong tuong nham dang dao.
    snprintf(turnLine, sizeof(turnLine), "DAO TRUNG: TIM GOC");
  } else if (currentRuntime.turnState == TurnState::Left ||
            currentRuntime.turnState == TurnState::Right) {
    snprintf(turnLine, sizeof(turnLine), "DAO TRUNG: %s",
             turnStateShort(currentRuntime.turnState));
  } else if (currentRuntime.turnState == TurnState::Fault) {
    snprintf(turnLine, sizeof(turnLine), "DAO TRUNG: LOI");
  } else if (currentRuntime.nextTurnScheduled) {
    snprintf(turnLine, sizeof(turnLine), "DAO TRUNG: %u PHUT",
             currentRuntime.nextTurnMinutes);
  } else {
    snprintf(turnLine, sizeof(turnLine), "DAO TRUNG: --");
  }
  lcd.setFont(u8g2_font_6x12_tf);
  drawCenteredText(58, turnLine);
}

void drawHome() {
  if (homePage == 0) drawHomeMain();
  else drawHomeOutputs();
}

void drawMainMenu() {
  drawHeader("MENU CHINH");
  lcd.setFont(u8g2_font_6x12_tf);
  for (uint8_t row = 0; row < 4 && listTop + row < MAIN_COUNT; ++row) {
    const uint8_t index = listTop + row;
    const int16_t y = 22 + row * 12;
    if (index == mainIndex) {
      lcd.drawBox(0, y - 9, 128, 11);
      lcd.setDrawColor(0);
      lcd.drawStr(2, y, ">");
      lcd.drawStr(12, y, mainItemLabel(index));
      lcd.setDrawColor(1);
    } else {
      lcd.drawStr(12, y, mainItemLabel(index));
    }
  }
}

void drawChungMenu() {
  drawHeader("CAI DAT CHUNG", false);
  drawListPosition(chungIndex, chungItemCount());
  lcd.setFont(u8g2_font_6x12_tf);
  for (uint8_t row = 0; row < 4 && chungTop + row < chungItemCount(); ++row) {
    const uint8_t index = chungTop + row;
    const int16_t y = 22 + row * 12;
    if (index == chungIndex) {
      lcd.drawBox(0, y - 9, 128, 11);
      lcd.setDrawColor(0);
      lcd.drawStr(2, y, ">");
      lcd.drawStr(12, y, chungItemLabel(index));
      lcd.setDrawColor(1);
    } else {
      lcd.drawStr(12, y, chungItemLabel(index));
    }
  }
}

void drawVentilationMenu() {
  const uint8_t count = ventilationMenuCount();
  if (ventilationIndex >= count) ventilationIndex = 0U;
  drawHeader("THONG GIO", false);
  drawListPosition(ventilationIndex, count);
  lcd.setFont(u8g2_font_6x12_tf);
  for (uint8_t i = 0U; i < count; ++i) {
    const int16_t y = 22 + i * 12;
    char row[24];
    if (i == 0U) snprintf(row, sizeof(row), "Tu dong: %s",
                          currentConfig.ventAutoEnabled ? "BAT" : "TAT");
    else if (!currentConfig.ventAutoEnabled) snprintf(row, sizeof(row), "Thoat");
    else if (i == 1U) snprintf(row, sizeof(row), "Muc: %s",
                               OPT_VENT_LEVEL[currentConfig.ventProfileLevel]);
    else if (i == 2U) snprintf(row, sizeof(row), "Nang cao");
    else snprintf(row, sizeof(row), "Thoat");
    if (i == ventilationIndex) {
      lcd.drawBox(0, y - 9, 128, 11);
      lcd.setDrawColor(0);
    }
    lcd.drawStr(12, y, row);
    if (i == ventilationIndex) lcd.setDrawColor(1);
  }
}

void drawVentilationAdvanced() {
  if (ventilationAdvancedIndex > 7U) ventilationAdvancedIndex = 0U;
  drawHeader("TG NANG CAO", false);
  drawListPosition(ventilationAdvancedIndex, 8U);
  lcd.setFont(u8g2_font_6x12_tf);
  const uint8_t top = ventilationAdvancedIndex < 4U ? 0U :
                      ventilationAdvancedIndex - 3U;
  for (uint8_t row = 0U; row < 4U; ++row) {
    const uint8_t index = top + row;
    const int16_t y = 22 + row * 12;
    char text[28];
    if (index < 7U) {
      const SettingItem &item = SETTINGS[44U + index];
      char value[12];
      formatSettingValue(item, readSetting(currentConfig, item), value, sizeof(value));
      snprintf(text, sizeof(text), "%s %s", index == 0U ? "Chu ky" : item.label,
               value);
    } else snprintf(text, sizeof(text), "Thoat");
    if (index == ventilationAdvancedIndex) {
      lcd.drawBox(0, y - 9, 128, 11);
      lcd.setDrawColor(0);
    }
    lcd.drawStr(12, y, text);
    if (index == ventilationAdvancedIndex) lcd.setDrawColor(1);
  }
}

void drawFirmwareMenu() {
  const uint8_t count = firmwareMenuCount();
  if (firmwareMenuIndex >= count) firmwareMenuIndex = count - 1U;
  drawHeader("CAP NHAT", false);
  drawListPosition(firmwareMenuIndex, count);
  lcd.setFont(u8g2_font_6x12_tf);
  for (uint8_t i = 0U; i < count; ++i) {
    const int16_t y = 22 + i * 12;
    char text[25];
    if (i == 0U) snprintf(text, sizeof(text), "Hien tai v%s", MAYAP_FIRMWARE_VERSION);
    else if (i == 1U) snprintf(text, sizeof(text), "Kiem tra cap nhat");
    else if (mayapRollbackAvailable() && i == 2U)
      snprintf(text, sizeof(text), "Quay lai ban cu");
    else snprintf(text, sizeof(text), "Thoat");
    if (i == firmwareMenuIndex) {
      lcd.drawBox(0, y - 9, 128, 11);
      lcd.setDrawColor(0);
    }
    lcd.drawStr(12, y, text);
    if (i == firmwareMenuIndex) lcd.setDrawColor(1);
  }
}

void drawSettingList() {
  const SettingGroup &group = GROUPS[selectedGroup];
  const uint8_t itemCount = settingListItemCount(selectedGroup);
  const uint8_t exitIndex = settingListExitIndex(selectedGroup);
  drawHeader(group.label, false);
  drawListPosition(listIndex, itemCount);
  lcd.setFont(u8g2_font_6x12_tf);
  char value[18];
  for (uint8_t row = 0; row < 4 && listTop + row < itemCount; ++row) {
    const uint8_t local = listTop + row;
    const int16_t y = 22 + row * 12;
    const bool selected = local == listIndex;
    if (selected) {
      lcd.drawBox(0, y - 9, 128, 11);
      lcd.setDrawColor(0);
    }

    if (local < group.count) {
      const uint8_t settingIndex = GROUP_SETTING_INDEXES[group.first + local];
      const SettingItem &item = SETTINGS[settingIndex];
      if (settingLockedDuringBatch(settingIndex)) snprintf(value, sizeof(value), "KHOA");
      else formatSettingValue(item, readSetting(currentConfig, item), value, sizeof(value));
      // Ve GIA TRI truoc (can phai, khong bi ep vao cot cung x=86 nhu truoc),
      // roi moi ve NHAN vao dung phan con lai voi khe ho 4px. Cach cu ep gia
      // tri bat dau tu x=86 co dinh nen nhan dai nhat ("Che do ket noi", ket
      // thuc dung x=86) dinh sat vao gia tri, nhin nhu bi cham chu.
      lcd.setFont(u8g2_font_6x12_tf);
      const int16_t valueW = static_cast<int16_t>(lcd.getStrWidth(value));
      const int16_t valueX = max(2, 126 - valueW);
      lcd.drawStr(valueX, y, value);
      // Dung BE RONG GIA TRI RONG NHAT CO THE (khong phai gia tri DANG hien)
      // de tinh ranh gioi cho nhan - tranh nhan tu doi co chu moi lan gia
      // tri thay doi do rong (vd "Che do ket noi": nhan vua khop khi la
      // "ONLINE" nhung "OFFLINE" dai hon 1 ky tu lam drawLeftFit2 tuong
      // khong du cho, tu dong lui ve font nho hon - nguoi dung thay day la
      // "man hinh tu doi font"; hoac "Chu ky dao" 1-720 phut, "1 ph" va
      // "720 ph" rong khac han nhau). Voi setting kieu lua chon (options),
      // gia tri rong nhat la lua chon dai nhat trong danh sach; voi setting
      // dang so, la gia tri o 1 trong 2 dau khoang (minimum/maximum - vd
      // dau am cua "-5.0" hoac nhieu chu so hon cua "720").
      int16_t widestValueW = 0;
      if (item.options && item.optionCount > 0U) {
        for (uint8_t optionIndex = 0; optionIndex < item.optionCount; ++optionIndex) {
          const int16_t w = static_cast<int16_t>(lcd.getStrWidth(item.options[optionIndex]));
          if (w > widestValueW) widestValueW = w;
        }
      } else {
        char probe[18];
        formatSettingValue(item, item.minimum, probe, sizeof(probe));
        widestValueW = static_cast<int16_t>(lcd.getStrWidth(probe));
        formatSettingValue(item, item.maximum, probe, sizeof(probe));
        const int16_t maxEndW = static_cast<int16_t>(lcd.getStrWidth(probe));
        if (maxEndW > widestValueW) widestValueW = maxEndW;
      }
      const int16_t labelBoundaryX = max(2, 126 - widestValueW);
      drawLeftFit2(2, y, item.label, static_cast<int16_t>(labelBoundaryX - 4),
                   u8g2_font_6x12_tf, u8g2_font_5x8_tf);
    } else if (local >= group.count && local < group.count + groupVisibleExtraCount(selectedGroup)) {
      // Dong muc phu (thong tin ket noi/doi wifi/thong ke dao) can trai dong
      // bo voi cac muc cai dat.
      const GroupExtra extra = visibleGroupExtraAt(selectedGroup,
          static_cast<uint8_t>(local - group.count));
      lcd.drawStr(2, y, groupExtraLabelFor(extra));
    } else if (local == exitIndex) {
      // Nut thoat cuon xuong cuoi danh sach, can trai nhu cac dong con lai.
      lcd.drawStr(2, y, "Thoat");
    }

    if (selected) lcd.setDrawColor(1);
  }
}

// Chuyen do manh Wi-Fi (dBm) thanh so vach de nguoi dung khong quen thang do
// dBm van doc duoc nhanh: 4 vach = rat manh, 1 vach = rat yeu, 0 = mat song.
uint8_t rssiToBars(int8_t dbm) {
  if (dbm <= -90) return 0U;
  if (dbm <= -80) return 1U;
  if (dbm <= -70) return 2U;
  if (dbm <= -60) return 3U;
  return 4U;
}

void drawConnectionInfo() {
  char text[28];
  drawHeader("KET NOI", false);
  lcd.setFont(u8g2_font_6x12_tf);

  // ID may: dung chung ham voi network_service.h (mayapDeviceIdText())
  // thay vi doc lai bien deviceId cua transaction_bridge.h - file nay duoc
  // include TRUOC transaction_bridge.h trong .ino nen bien do chua khai bao
  // luc bien dich hmi.h (xem comment tai mayapDeviceIdText()).
  snprintf(text, sizeof(text), "ID: %s", mayapDeviceIdText().c_str());
  drawLeftFit(6, 20, text, u8g2_font_6x12_tf, u8g2_font_5x8_tf,
              u8g2_font_5x8_tf);

  if (currentRuntime.networkConnected) {
    snprintf(text, sizeof(text), "WIFI: DA KET NOI");
  } else {
    snprintf(text, sizeof(text), "WIFI: CHUA KET NOI");
  }
  drawLeftFit(6, 31, text, u8g2_font_5x8_tf, u8g2_font_5x8_tf,
              u8g2_font_5x8_tf);

  if (currentRuntime.networkConnected) {
    const int32_t dbm = currentRuntime.networkRssiDbm;
    snprintf(text, sizeof(text), "SONG: %ld dBm (%u/4)",
             static_cast<long>(dbm), rssiToBars(static_cast<int8_t>(constrain(dbm, -127L, 0L))));
  } else snprintf(text, sizeof(text), "SONG: CHUA KET NOI");
  drawLeftFit(6, 43, text, u8g2_font_5x8_tf, u8g2_font_5x8_tf,
              u8g2_font_5x8_tf);

  // Khach chi can ID + PIN nay; WSS va token cloud duoc cap tu dong.
  snprintf(text, sizeof(text), "PIN WEB: %s", mayapWebPinText());

  drawLeftFit(6, 55, text, u8g2_font_5x8_tf, u8g2_font_5x8_tf,
              u8g2_font_5x8_tf);
}

void drawQrCode() {
  // Device ID khong doi trong suot phien chay (tinh 1 lan tu MAC luc khoi
  // dong) nen chi ma hoa QR MOT LAN duy nhat, khong lam lai moi khung hinh.
  static bool matrix[QrMapId::SIZE][QrMapId::SIZE];
  static bool ok = false;
  static bool computed = false;
  if (!computed) {
    ok = QrMapId::encode(mayapDeviceIdText().c_str(), matrix);
    computed = true;
  }
  if (!ok) {
    drawHeader("MA QR ID", false);
    drawCenteredFit(36, "LOI TAO MA QR", u8g2_font_6x12_tf, u8g2_font_5x8_tf,
                    u8g2_font_5x8_tf);
    return;
  }

  // QR sat canh PHAI, chiem tron chieu cao 64px (khong con le tren/duoi -
  // du thua cua phep chia nguyen don het xuong duoi thay vi chia deu 2 ben,
  // de canh tren QR cham dinh man hinh). Cot con lai ben TRAI danh cho
  // tieu de + ID may (giong nhan thiet bi that: 1 dong huong dan, gach
  // phan cach, roi ma so) - vua giup nguoi dung biet man nay dung de lam
  // gi, vua cho doi chieu bang mat khi khong tien quet. pxSize=2 la toi da
  // co the tren man 64px cao voi vanh trang chuan 4-module bat buoc de dau
  // doc dinh vi duoc (xem giai thich chi tiet tai namespace QrMapId dau
  // file). Thoat man hinh van bang short-press nhu moi man phu khac.
  constexpr uint8_t QUIET = 4;   // vanh trang toi thieu theo chuan QR
  constexpr uint8_t PX = 2;      // pixel LCD / 1 o QR
  constexpr uint8_t TOTAL_MODULES = QrMapId::SIZE + QUIET * 2;
  constexpr uint8_t TOTAL_PX = TOTAL_MODULES * PX;  // 58
  constexpr int16_t OFFSET_X = 128 - TOTAL_PX;      // sat canh phai (x=128)
  constexpr int16_t OFFSET_Y = 0;                   // sat canh tren

  lcd.setDrawColor(1);
  for (uint8_t r = 0; r < QrMapId::SIZE; ++r) {
    for (uint8_t c = 0; c < QrMapId::SIZE; ++c) {
      if (!matrix[r][c]) continue;
      const int16_t x = OFFSET_X + (QUIET + c) * PX;
      const int16_t y = OFFSET_Y + (QUIET + r) * PX;
      lcd.drawBox(x, y, PX, PX);
    }
  }

  // Cot trai (rong OFFSET_X = 70px): tieu de 1 dong NGANG, gach phan cach,
  // roi ID may chia 2 dong 8 ky tu (vua khop font 6x12 trong 70px, de doc
  // hon xep doc tung chu roi rac truoc day). Can giua CUA RIENG COT DO
  // (khong phai giua ca man 128px nhu drawCenteredFit() mac dinh).
  const int16_t labelMaxWidth = static_cast<int16_t>(OFFSET_X - 4);
  const auto drawColCentered = [&](int16_t y, const char *text) {
    const int16_t w = fitFontWidth(text, labelMaxWidth, u8g2_font_6x12_tf,
                                   u8g2_font_6x12_tf, u8g2_font_5x8_tf);
    lcd.drawStr(max(0, (OFFSET_X - w) / 2), y, text);
  };

  drawColCentered(14, "QUET MA QR");
  lcd.drawHLine(6, 19, OFFSET_X - 12);

  // Device ID luon dung 16 ky tu hop le tai day (QrMapId::encode() da tra
  // ve true o tren, nen khong can kiem tra lai) - chia doi 8+8 de vua font.
  const String idStr = mayapDeviceIdText();
  char idLine1[9];
  char idLine2[9];
  snprintf(idLine1, sizeof(idLine1), "%.8s", idStr.c_str());
  snprintf(idLine2, sizeof(idLine2), "%.8s", idStr.c_str() + 8);
  drawColCentered(38, idLine1);
  drawColCentered(52, idLine2);
}

void drawTurnStats() {
  char text[28];
  drawHeader("SO LAN DAO", false);
  lcd.setFont(u8g2_font_6x12_tf);

  // Cac dong nay chua so dem co the rat dai (turnCountBatch la uint32) - di
  // qua drawLeftFit de du du lieu bat thuong cung khong tran ra ngoai man.
  snprintf(text, sizeof(text), "HOM NAY: %u LAN",
           currentRuntime.turnCountToday);
  drawLeftFit(6, 29, text, u8g2_font_6x12_tf, u8g2_font_5x8_tf,
              u8g2_font_5x8_tf);

  snprintf(text, sizeof(text), "TONG ME: %lu LAN",
           static_cast<unsigned long>(currentRuntime.turnCountBatch));
  drawLeftFit(6, 43, text, u8g2_font_6x12_tf, u8g2_font_5x8_tf,
              u8g2_font_5x8_tf);

  if (currentRuntime.turningLockdown) snprintf(text, sizeof(text), "DAO TIEP: KHOA");
  else if (currentRuntime.nextTurnScheduled) snprintf(text, sizeof(text), "DAO TIEP: %u PH", currentRuntime.nextTurnMinutes);
  else snprintf(text, sizeof(text), "DAO TIEP: --");
  drawLeftFit(6, 58, text, u8g2_font_5x8_tf, u8g2_font_5x8_tf,
              u8g2_font_5x8_tf);
}

void drawEditSetting() {
  const SettingItem &item = SETTINGS[editSettingIndex];
  char value[24];
  // "SUA THONG SO" (khong phai "CHINH THONG SO") - tieu de cu dai 14 ky tu,
  // vuot gioi han cat chuoi 13 ky tu cua drawHeader() nen bi rung mat chu
  // "O" cuoi cung ("...THONG S"), day chinh la loi nguoi dung bao.
  drawHeader("SUA THONG SO", false);
  drawCenteredFit(23, item.label, u8g2_font_6x12_tf, u8g2_font_5x8_tf,
                  u8g2_font_5x8_tf);
  formatSettingValue(item, editValue, value, sizeof(value));

  // Bo icon cong tac do hoa (toggle switch) khoi toan bo HMI - moi muc Bool
  // (ke ca "Ap lai") deu chi hien chu "BAT"/"TAT" to, ro rang, dong nhat
  // cach hien thi voi cac loai gia tri khac (khong con nhanh rieng).
  drawCenteredFit(48, value, u8g2_font_helvB14_tf, u8g2_font_helvB12_tf,
                  u8g2_font_6x12_tf);
  lcd.setFont(u8g2_font_5x8_tf);
  lcd.drawStr(11, 62, "NHAN: LUU   GIU: HUY");
}

const char *autoTuneStateText(AutoTuneState state) {
  switch (state) {
    case AutoTuneState::Running: return "DANG TU CHINH";
    case AutoTuneState::Success: return "DA HOAN THANH";
    case AutoTuneState::Failed: return "THAT BAI";
    default: return "SAN SANG";
  }
}

void drawAutoTune() {
  char text[28];
  drawHeader("TU CHINH PID", false);
  drawCenteredFit(29, autoTuneStateText(currentRuntime.autoTuneState),
                  u8g2_font_helvB12_tf, u8g2_font_6x12_tf, u8g2_font_5x8_tf);

  if (currentRuntime.autoTuneState == AutoTuneState::Running) {
    snprintf(text, sizeof(text), "TIEN DO %u%%",
             currentRuntime.autoTuneProgress);
  } else if (currentRuntime.batchRunning) {
    snprintf(text, sizeof(text), "HAY DUNG ME TRUOC");
  } else if (!currentRuntime.sensorOnline) {
    snprintf(text, sizeof(text), "CAM BIEN DANG LOI");
  } else {
    snprintf(text, sizeof(text), "MAY SE TU TIM PID");
  }
  drawCenteredFit(46, text, u8g2_font_6x12_tf, u8g2_font_5x8_tf,
                  u8g2_font_5x8_tf);
}

const char *testOutputLabel(uint8_t index) {
  switch (static_cast<TestOutputId>(index)) {
    case TestOutputId::HeaterSsr: return "SSR NHIET";
    case TestOutputId::HeatMaster: return "CONTACTOR TONG";
    case TestOutputId::CirculationFan: return "QUAT TUAN HOAN";
    case TestOutputId::VentFan: return "QUAT HUT";
    case TestOutputId::Light: return "DEN";
    case TestOutputId::Siren: return "COI";
    case TestOutputId::TurnLeft: return "DAO TRAI";
    case TestOutputId::TurnRight: return "DAO PHAI";
    default: return "?";
  }
}

const char *testLimitPhaseText(TestLimitPhase phase) {
  switch (phase) {
    case TestLimitPhase::Waiting: return "DANG CHO TAC DONG";
    case TestLimitPhase::Success: return "DA PHAT HIEN - OK";
    case TestLimitPhase::Timeout: return "KHONG PHAT HIEN";
    default: return "";
  }
}

const char *testResultTag(TestResult result) {
  switch (result) {
    case TestResult::Pass: return "OK";
    case TestResult::Fail: return "KHONG";
    default: return "...";
  }
}

// 2 nut lon dung chung cho moi man xac nhan Co/Khong tren HMI (dung lai o
// day va drawConfirmScreen() ben duoi) - chu nhan nam gon giua o, o bo goc
// tron cho mem mat, de nhan dien nhanh.
void drawYesNoButtons(bool yesSelected, const char *yesLabel, const char *noLabel) {
  constexpr int16_t btnY = 40, btnH = 20, margin = 4, gap = 6, radius = 4;
  constexpr int16_t btnW = (128 - 2 * margin - gap) / 2;
  static_assert(btnW >= 2 * radius + 1 && btnH >= 2 * radius + 1,
                "Nut qua nho so voi ban kinh bo goc (drawRBox se ve sai)");
  const int16_t yesX = margin;
  const int16_t noX = margin + btnW + gap;

  // Font nho hon truoc (helvB10 thay helvB12) va can giua CA HAI CHIEU trong
  // o: baseline tinh theo getAscent() cua chinh font dang dung thay vi hang
  // so btnY+15 co dinh - doi font sau nay van tu can dung, khong bi lech.
  lcd.setFont(u8g2_font_helvB10_tf);
  const int16_t ascent = static_cast<int16_t>(lcd.getAscent());
  const int16_t baseline = btnY + (btnH + ascent) / 2;

  lcd.setDrawColor(1);
  if (yesSelected) lcd.drawRBox(yesX, btnY, btnW, btnH, radius);
  else lcd.drawRFrame(yesX, btnY, btnW, btnH, radius);
  lcd.setDrawColor(yesSelected ? 0 : 1);
  {
    const int16_t tw = static_cast<int16_t>(lcd.getStrWidth(yesLabel));
    lcd.drawStr(yesX + max(0, (btnW - tw) / 2), baseline, yesLabel);
  }

  lcd.setDrawColor(1);
  if (!yesSelected) lcd.drawRBox(noX, btnY, btnW, btnH, radius);
  else lcd.drawRFrame(noX, btnY, btnW, btnH, radius);
  lcd.setDrawColor(yesSelected ? 1 : 0);
  {
    const int16_t tw = static_cast<int16_t>(lcd.getStrWidth(noLabel));
    lcd.drawStr(noX + max(0, (btnW - tw) / 2), baseline, noLabel);
  }
  lcd.setDrawColor(1);
}

void drawTestDeviceConfirm() {
  drawHeader("TEST");
  lcd.setFont(u8g2_font_6x12_tf);
  drawCenteredText(24, testOutputLabel(testDeviceConfirmIndex));
  drawCenteredText(36, "CO CHAY KHONG?");
  drawYesNoButtons(testDeviceConfirmYes, "CO", "KHONG");
}

void drawHeaterTestWorkflow() {
  drawHeader("TEST NHIET", false);
  lcd.setFont(u8g2_font_6x12_tf);
  if (heaterTestUiPhase == HeaterTestUiPhase::Heating) {
    const uint32_t now = millis();
    const uint32_t elapsed = now - heaterTestUiStartedAt;
    char line[28];
    if (elapsed < TEST_HEATER_FAN_PRESTART_MS) {
      const uint32_t left = (TEST_HEATER_FAN_PRESTART_MS - elapsed + 999UL) / 1000UL;
      snprintf(line, sizeof(line), "QUAT KHOI DONG %lus", static_cast<unsigned long>(left));
      drawCenteredFit(24, line, u8g2_font_6x12_tf, u8g2_font_5x8_tf, u8g2_font_5x8_tf);
    } else {
      const uint32_t heatElapsed = elapsed - TEST_HEATER_FAN_PRESTART_MS;
      const uint32_t leftSec = heatElapsed >= TEST_HEATER_HOLD_MAX_MS ? 0U :
          (TEST_HEATER_HOLD_MAX_MS - heatElapsed + 999UL) / 1000UL;
      snprintf(line, sizeof(line), "CON LAI %02lu:%02lu",
               static_cast<unsigned long>(leftSec / 60UL),
               static_cast<unsigned long>(leftSec % 60UL));
      drawCenteredFit(24, line, u8g2_font_6x12_tf, u8g2_font_5x8_tf, u8g2_font_5x8_tf);
    }
    if (currentRuntime.sensorOnline) snprintf(line, sizeof(line), "NHIET %.1fC", currentRuntime.temperature);
    else snprintf(line, sizeof(line), "CAM BIEN LOI");
    drawCenteredFit(40, line, u8g2_font_helvB12_tf, u8g2_font_6x12_tf, u8g2_font_5x8_tf);
    drawCenteredFit(58, "NHAN = DUNG SOM", u8g2_font_5x8_tf,
                    u8g2_font_5x8_tf, u8g2_font_5x8_tf);
    return;
  }
  if (heaterTestUiPhase == HeaterTestUiPhase::ConfirmHeat) {
    drawCenteredFit(29, "NHIET DA LEN CHUA?", u8g2_font_6x12_tf,
                    u8g2_font_5x8_tf, u8g2_font_5x8_tf);
    drawYesNoButtons(heaterTestConfirmYes, "CO", "KHONG");
    return;
  }
  drawCenteredFit(29, "QUAT HUT DA CHAY?", u8g2_font_6x12_tf,
                  u8g2_font_5x8_tf, u8g2_font_5x8_tf);
  drawYesNoButtons(heaterTestConfirmYes, "CO", "KHONG");
}

void drawTestMode() {
  if (heaterTestUiPhase != HeaterTestUiPhase::Idle) { drawHeaterTestWorkflow(); return; }
  if (testDeviceConfirmActive) { drawTestDeviceConfirm(); return; }
  drawHeader("TEST");
  lcd.setFont(u8g2_font_6x12_tf);
  char line[24];
  for (uint8_t row = 0; row < 4 && listTop + row < TEST_MODE_ITEM_COUNT; ++row) {
    const uint8_t local = listTop + row;
    const int16_t y = 22 + row * 12;
    const bool selected = local == listIndex;
    if (selected) {
      lcd.drawBox(0, y - 9, 128, 11);
      lcd.setDrawColor(0);
    }
    if (local < TEST_MODE_OUTPUT_ROWS) {
      const bool on = (currentRuntime.testOutputMaskActive & (1U << local)) != 0U;
      const char *tag = on ? "BAT" : testResultTag(testDeviceResult[local]);
      lcd.drawStr(2, y, testOutputLabel(local));
      lcd.drawStr(max(80, 126 - static_cast<int16_t>(lcd.getStrWidth(tag))), y, tag);
    } else if (local < TEST_MODE_OUTPUT_ROWS + TEST_MODE_LIMIT_ROWS) {
      const uint8_t limitIdx = static_cast<uint8_t>(local - TEST_MODE_OUTPUT_ROWS);
      const TestLimitId id = static_cast<TestLimitId>(limitIdx);
      snprintf(line, sizeof(line), "CTHT %s", id == TestLimitId::Left ? "TRAI" : "PHAI");
      lcd.drawStr(2, y, line);
      const char *tag = (currentRuntime.testLimitTarget == id &&
                         currentRuntime.testLimitPhase == TestLimitPhase::Waiting)
          ? "..." : testResultTag(testLimitResult[limitIdx]);
      lcd.drawStr(max(80, 126 - static_cast<int16_t>(lcd.getStrWidth(tag))), y, tag);
    } else {
      // Doi tu "Thoat" sang "Ket thuc": bam vao day de xem tong ket thiet bi
      // nao da chay, thiet bi nao chua/khong chay truoc khi roi han.
      lcd.drawStr(2, y, "Ket thuc");
    }
    if (selected) lcd.setDrawColor(1);
  }
}

void drawTestSummary() {
  drawHeader("TONG KET TEST");
  lcd.setFont(u8g2_font_6x12_tf);
  char line[24];
  for (uint8_t row = 0; row < 4 && testSummaryIndex / 4 * 4 + row < TEST_SUMMARY_ITEM_COUNT; ++row) {
    const uint8_t local = static_cast<uint8_t>(testSummaryIndex / 4 * 4 + row);
    const int16_t y = 22 + row * 12;
    const bool selected = local == testSummaryIndex;
    if (selected) {
      lcd.drawBox(0, y - 9, 128, 11);
      lcd.setDrawColor(0);
    }
    const char *tag;
    if (local < TEST_MODE_OUTPUT_ROWS) {
      lcd.drawStr(2, y, testOutputLabel(local));
      tag = testResultTag(testDeviceResult[local]);
    } else {
      const uint8_t limitIdx = static_cast<uint8_t>(local - TEST_MODE_OUTPUT_ROWS);
      snprintf(line, sizeof(line), "CTHT %s", limitIdx == 0U ? "TRAI" : "PHAI");
      lcd.drawStr(2, y, line);
      tag = testResultTag(testLimitResult[limitIdx]);
    }
    lcd.drawStr(max(80, 126 - static_cast<int16_t>(lcd.getStrWidth(tag))), y, tag);
    if (selected) lcd.setDrawColor(1);
  }
}

// Man hinh CHIEM TOAN BO man hinh trong luc dang tai/flash firmware moi
// (View::FirmwareProgress) - khong the thoat/bam gi trong luc applyPhase==1
// (xem chan input o dau handleRotary()), giong san pham thuong mai: nguoi
// dung biet ro may dang lam gi va KHONG duoc tat nguon giua chung.
void drawFirmwareProgress() {
  const FirmwareWebStatus status = mayapFirmwareWebStatus();
  // Header toi da 13 ky tu (xem drawHeader()) - "CAP NHAT FIRMWARE" se bi cat
  // mat chu cuoi giong loi da gap truoc day voi "SUA THONG SO", nen dung ten
  // ngan "CAP NHAT".
  drawHeader("CAP NHAT", false);
  lcd.setFont(u8g2_font_6x12_tf);

  if (status.applyPhase == 2U) {
    drawCenteredFit(29, "THAT BAI", u8g2_font_helvB12_tf, u8g2_font_6x12_tf,
                    u8g2_font_6x12_tf);
    drawLeftFit(4, 45, status.lastError, u8g2_font_5x8_tf, u8g2_font_5x8_tf,
                u8g2_font_5x8_tf);
    drawCenteredFit(60, "BAM DE THOAT", u8g2_font_5x8_tf, u8g2_font_5x8_tf,
                    u8g2_font_5x8_tf);
    return;
  }
  if (status.applyPhase == 3U) {
    // Thuc te hau nhu khong kip ve khung hinh nay - ESP.restart() goi ngay
    // sau khi dat phase=3 (delay 300ms), nhung van ve cho day du trang thai.
    drawCenteredFit(38, "THANH CONG!", u8g2_font_helvB12_tf,
                    u8g2_font_6x12_tf, u8g2_font_6x12_tf);
    return;
  }

  // applyPhase == 1 (dang tai) - hoac 0 neu vua vao man hinh nay truoc khi
  // otaTask kip chuyen phase, van ve % = 0 cho khong bi giat hinh.
  char text[24];
  snprintf(text, sizeof(text), "%u%%", status.downloadPercent);
  drawCenteredFit(34, text, u8g2_font_helvB14_tf, u8g2_font_helvB12_tf,
                  u8g2_font_6x12_tf);

  // Thanh % dang pixel don gian, khung 108x8 tai (10,42).
  lcd.drawFrame(10, 42, 108, 8);
  const uint16_t fillWidth = (108U * status.downloadPercent) / 100U;
  if (fillWidth > 0U) lcd.drawBox(10, 42, static_cast<uint16_t>(fillWidth), 8);

  drawCenteredFit(60, "KHONG TAT MAY", u8g2_font_5x8_tf, u8g2_font_5x8_tf,
                  u8g2_font_5x8_tf);
}

void drawWifiChange() {
  drawHeader("DOI WIFI");
  lcd.setFont(u8g2_font_6x12_tf);
  const WifiPortalState state = currentRuntime.wifiPortalState;
  const char *title = "DANG MO CONG...";
  if (state == WifiPortalState::ApActive) title = "KET NOI VA DOI WIFI";
  else if (state == WifiPortalState::Testing) title = "DANG THU KET NOI...";
  else if (state == WifiPortalState::Success) title = "DA KET NOI!";
  else if (state == WifiPortalState::Failed) title = "KHONG THE KET NOI";
  drawCenteredFit(24, title, u8g2_font_6x12_tf, u8g2_font_5x8_tf,
                  u8g2_font_5x8_tf);

  // Cac dong huong dan ben duoi truoc day ve thang bang drawStr() o font
  // 6x12/5x8 co dinh, mot so cau dai toi ~170px (vd "Hay thu lai voi mang/
  // mat khau khac") nen bi cat cut o mep phai. Gio tach thanh 2 dong ngan va
  // van di qua drawCenteredFit() de chac chan khong bao gio tran.
  char line[40];
  if (state == WifiPortalState::ApActive || state == WifiPortalState::Testing) {
    snprintf(line, sizeof(line), "WiFi: %s", currentRuntime.wifiPortalApName);
    drawCenteredFit(38, line, u8g2_font_5x8_tf, u8g2_font_5x8_tf,
                    u8g2_font_5x8_tf);
    snprintf(line, sizeof(line), "MK: %s", currentRuntime.wifiPortalPassword);
    drawCenteredFit(50, line, u8g2_font_5x8_tf, u8g2_font_5x8_tf,
                    u8g2_font_5x8_tf);
  } else if (state == WifiPortalState::Failed) {
    drawCenteredFit(38, "Hay thu lai voi mang", u8g2_font_5x8_tf,
                    u8g2_font_5x8_tf, u8g2_font_5x8_tf);
    drawCenteredFit(50, "hoac mat khau khac", u8g2_font_5x8_tf,
                    u8g2_font_5x8_tf, u8g2_font_5x8_tf);
  } else if (state == WifiPortalState::Success) {
    drawCenteredFit(38, "May da chuyen sang", u8g2_font_5x8_tf,
                    u8g2_font_5x8_tf, u8g2_font_5x8_tf);
    drawCenteredFit(50, "Wi-Fi moi", u8g2_font_5x8_tf, u8g2_font_5x8_tf,
                    u8g2_font_5x8_tf);
  }
}

void formatEventAge(uint32_t ageSec, char *out, size_t size) {
  if (ageSec < 5U) {
    snprintf(out, size, "VUA XONG");
  } else if (ageSec < 60U) {
    snprintf(out, size, "CACH %lus", static_cast<unsigned long>(ageSec));
  } else {
    snprintf(out, size, "CACH %lup%02lus",
             static_cast<unsigned long>(ageSec / 60UL),
             static_cast<unsigned long>(ageSec % 60UL));
  }
}

const char *inputEventName(uint16_t code) {
  switch (code - 100U) {
    case 0: return "HT TRAI";
    case 1: return "HT PHAI";
    case 2: return "CHE DO AUTO";
    case 3: return "CONG TAC NHIET";
    case 4: return "CONG TAC QUAT";
    case 5: return "CONG TAC DEN";
    case 6: return "LENH DAO TRAI";
    case 7: return "LENH DAO PHAI";
    default: return "INPUT";
  }
}

const char *outputEventName(uint16_t code) {
  switch (code - 200U) {
    case 1: return "CONTACTOR NHIET";
    case 2: return "DAO TRAI";
    case 3: return "DAO PHAI";
    case 4: return "QUAT HUT";
    case 5: return "DEN";
    case 6: return "QUAT TUAN HOAN";
    case 7: return "COI KHAN";
    default: return "OUTPUT";
  }
}

void eventText(const HmiEventItem &e, char *title, size_t titleSize,
               char *detail, size_t detailSize) {
  title[0] = detail[0] = '\0';
  if (e.code >= 1100U && e.code < 2000U) {
    const uint16_t faultCode = static_cast<uint16_t>(e.code - 1000U);
    snprintf(title, titleSize, "E%03u %s", faultCode, faultTitle(faultCode));
    switch (e.type) {
      case 4: snprintf(detail, detailSize, "LOI XUAT HIEN"); break;
      case 5: snprintf(detail, detailSize, "LOI DA HET"); break;
      case 6: snprintf(detail, detailSize, "NGUOI DUNG DA XN"); break;
      default: snprintf(detail, detailSize, "CHI TIET %d", e.value); break;
    }
    return;
  }
  if (e.code >= 100U && e.code < 200U) {
    snprintf(title, titleSize, "%s", inputEventName(e.code));
    snprintf(detail, detailSize, "%s", e.value ? "BAT" : "TAT");
    return;
  }
  if (e.code >= 200U && e.code < 300U) {
    snprintf(title, titleSize, "%s", outputEventName(e.code));
    snprintf(detail, detailSize, "%s", e.value ? "BAT" : "TAT");
    return;
  }
  switch (e.code) {
    case 1: snprintf(title, titleSize, "KHOI DONG NGUON"); break;
    case 2: snprintf(title, titleSize, "RESET NGOAI"); break;
    case 3: snprintf(title, titleSize, "RESET PHAN MEM"); break;
    case 4: snprintf(title, titleSize, "RESET PANIC"); break;
    case 5: snprintf(title, titleSize, "RESET WATCHDOG"); break;
    case 6: snprintf(title, titleSize, "RESET BROWNOUT"); break;
    case 20: snprintf(title, titleSize, "BAT DAU ME AP"); break;
    case 21: snprintf(title, titleSize, "DUNG ME AP"); break;
    case 22: snprintf(title, titleSize, "PHUC HOI ME"); break;
    case 23: snprintf(title, titleSize, "CHO XN SAU MAT DIEN"); break;
    case 24: snprintf(title, titleSize, "DA TIEP TUC ME CU"); break;
    case 25: snprintf(title, titleSize, "DA HUY ME CU"); break;
    case 30: snprintf(title, titleSize, "CHUYEN SANG AUTO"); break;
    case 31: snprintf(title, titleSize, "CHUYEN SANG MANUAL"); break;
    case 32: snprintf(title, titleSize, "AUTO OFF GIUA ME"); break;
    case 33: snprintf(title, titleSize, "AUTO DA PHUC HOI"); break;
    case 40: snprintf(title, titleSize, "CAM BIEN PHUC HOI"); break;
    case 41: snprintf(title, titleSize, "MAT CAM BIEN"); break;
    case 42: snprintf(title, titleSize, "RTC DA KET NOI LAI"); break;
    case 43: snprintf(title, titleSize, "RTC TU HIEU CHINH"); break;
    case 44: snprintf(title, titleSize, "RTC DONG BO NTP"); break;
    case 50: snprintf(title, titleSize, "DA LUU CAU HINH"); break;
    case 51: snprintf(title, titleSize, "BAT AUTO TUNE"); break;
    case 52: snprintf(title, titleSize, "AUTO TUNE OK"); break;
    case 53: snprintf(title, titleSize, "AUTO TUNE LOI"); break;
    case 451: snprintf(title, titleSize, "BO MO HINH NHIET"); return;
    case 452: snprintf(title, titleSize, "BAT TU CAN BANG"); return;
    case 453: snprintf(title, titleSize, "TAT TU CAN BANG"); return;
    case 450: snprintf(title, titleSize, "TU CAN BANG NHIET"); snprintf(detail, detailSize, "TRANG THAI %d", e.value); return;
    case 54: snprintf(title, titleSize, "EEPROM PHUC HOI"); break;
    case 55:
      snprintf(title, titleSize, "DA BU GIO MAT DIEN");
      snprintf(detail, detailSize, "MAT DIEN ~%d GIO", e.value);
      return;
    case 60: snprintf(title, titleSize, "BAT DAO TRAI"); break;
    case 61: snprintf(title, titleSize, "BAT DAO PHAI"); break;
    case 62: snprintf(title, titleSize, "TIM GOC TRAI"); break;
    case 63: snprintf(title, titleSize, "TIM GOC PHAI"); break;
    case 64: snprintf(title, titleSize, "DA DEN BEN TRAI"); break;
    case 65: snprintf(title, titleSize, "DA DEN BEN PHAI"); break;
    case 70: snprintf(title, titleSize, "MANG DA TAT"); break;
    case 71: snprintf(title, titleSize, "MANG DANG KET NOI"); break;
    case 72:
      snprintf(title, titleSize, "MANG DA KET NOI");
      snprintf(detail, detailSize, "TIN HIEU %d dBm", e.value);
      return;
    case 73: snprintf(title, titleSize, "MANG BI MAT"); break;
    case 74: snprintf(title, titleSize, "MANG CHUA CAU HINH"); break;
    case 80:
      snprintf(title, titleSize, "LENH BI TU CHOI");
      snprintf(detail, detailSize, "MA LENH %d", e.value);
      return;
    default: snprintf(title, titleSize, "SU KIEN %u", e.code); break;
  }
  if (e.value) snprintf(detail, detailSize, "GIA TRI %d", e.value);
  else snprintf(detail, detailSize, "DA GHI NHAN");
}

bool hmiLeapYear(uint16_t year) {
  return (year % 4U == 0U && year % 100U != 0U) || (year % 400U == 0U);
}

uint8_t hmiDaysInMonth(uint16_t year, uint8_t month) {
  static const uint8_t days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (month < 1U || month > 12U) return 30U;
  return static_cast<uint8_t>(days[month - 1U] +
      ((month == 2U && hmiLeapYear(year)) ? 1U : 0U));
}

void formatEventDateTime(uint32_t epoch, char *out, size_t size) {
  if (!out || size == 0U) return;
  if (epoch == 0U) {
    snprintf(out, size, "--/-- --:--:--");
    return;
  }
  uint32_t days = epoch / 86400UL;
  uint32_t seconds = epoch % 86400UL;
  uint16_t year = 1970U;
  while (year < 2107U) {
    const uint16_t yearDays = hmiLeapYear(year) ? 366U : 365U;
    if (days < yearDays) break;
    days -= yearDays;
    ++year;
  }
  uint8_t month = 1U;
  while (month <= 12U) {
    const uint8_t monthDays = hmiDaysInMonth(year, month);
    if (days < monthDays) break;
    days -= monthDays;
    ++month;
  }
  const uint8_t day = static_cast<uint8_t>(days + 1U);
  const uint8_t hour = static_cast<uint8_t>(seconds / 3600UL);
  const uint8_t minute = static_cast<uint8_t>((seconds % 3600UL) / 60UL);
  const uint8_t second = static_cast<uint8_t>(seconds % 60UL);
  snprintf(out, size, "%02u/%02u %02u:%02u:%02u",
           static_cast<unsigned>(day), static_cast<unsigned>(month),
           static_cast<unsigned>(hour), static_cast<unsigned>(minute),
           static_cast<unsigned>(second));
}

void drawEventLog() {
  drawHeader(eventLogFaultsOnly ? "LOI GAN DAY" : "NHAT KY ME");
  const uint8_t visibleCount = eventLogVisibleCount();
  if (!visibleCount) {
    drawCenteredFit(35,
                    eventLogFaultsOnly ? "KHONG CO LOI GAN DAY" : "CHUA CO SU KIEN",
                    u8g2_font_6x12_tf, u8g2_font_5x8_tf, u8g2_font_5x8_tf);
    return;
  }
  if (eventLogIndex >= visibleCount) eventLogIndex = 0U;
  const HmiEventItem *item = eventLogVisibleItem(eventLogIndex);
  if (!item) return;
  const HmiEventItem &e = *item;
  char age[18];
  char timestamp[24];
  char title[28];
  char detail[28];
  char footer[40];
  const uint32_t ageSec = e.ageSec + (eventLogFaultsOnly ?
      (millis() - recentFaultCapturedAt[eventLogIndex]) / 1000UL : 0U);
  formatEventAge(ageSec, age, sizeof(age));
  formatEventDateTime(e.epoch, timestamp, sizeof(timestamp));
  eventText(e, title, sizeof(title), detail, sizeof(detail));

  lcd.setFont(u8g2_font_5x8_tf);
  char top[40];
  snprintf(top, sizeof(top), "#%04lu %s",
           static_cast<unsigned long>(e.sequence % 10000UL), timestamp);
  lcd.drawStr(1, 20, top);
  // Tieu de su kien loi co dang "E113 NHIET DO BIEN THIEN NHANH" - dai hon
  // han 128px o font 6x12, truoc day bi cat mat duoi. Tu ha co font cho vua.
  drawLeftFit(1, 34, title, u8g2_font_6x12_tf, u8g2_font_5x8_tf,
              u8g2_font_5x8_tf);
  drawLeftFit(1, 46, detail, u8g2_font_5x8_tf, u8g2_font_5x8_tf,
              u8g2_font_5x8_tf);
  lcd.setFont(u8g2_font_5x8_tf);
  snprintf(footer, sizeof(footer), "%u/%u  %s",
           eventLogIndex + 1U,
           eventLogFaultsOnly ? visibleCount : currentEventLog.totalInWindow, age);
  lcd.drawStr(1, 59, footer);
}

// Man hinh xac nhan RIENG, chiem toan bo LCD - truoc day chi la 1 dai 9px o
// cuoi man de khong che noi dung, nhung qua nho de doc/de bam nham. Gio thay
// han noi dung man dang xem (xem render(): bo qua ve view khi dang confirm),
// nen co the dung het khong gian cho cau hoi + 2 nut CO/HUY to, ro rang.
void drawConfirmScreen() {
  drawHeader("XAC NHAN");

  const char *line1;
  const char *line2 = "";
  if (confirmAction == ConfirmAction::ResumeBatch) {
    line1 = "MAT DIEN - TIEP TUC";
    line2 = "ME DANG AP?";
  } else if (confirmAction == ConfirmAction::BatchOverdueContinue) {
    line1 = "QUA HAN NGAY AP";
    line2 = "TIEP TUC U AM?";
  } else if (confirmAction == ConfirmAction::AutoTuneStart) {
    line1 = "CHAY AUTO TUNE PID?";
  } else if (confirmAction == ConfirmAction::AutoTuneCancel) {
    line1 = "HUY TU DO PID?";
  } else if (confirmAction == ConfirmAction::TurningToggle) {
    line1 = pendingTurningConfig.turningEnabled ? "BAT DAO TU DONG?" : "TAT DAO TU DONG?";
  } else if (confirmAction == ConfirmAction::CloudPinReset) {
    line1 = "DAT LAI MA PIN WEB";
    line2 = "TAO MA 6 SO MOI?";
  } else if (confirmAction == ConfirmAction::FirmwareRollback) {
    line1 = "QUAY LAI FIRMWARE CU?";
    line2 = "MAY SE KHOI DONG LAI";
  } else if (confirmAction == ConfirmAction::FirmwareWebApply) {
    static char fwVerLine[24];
    snprintf(fwVerLine, sizeof(fwVerLine), "LEN v%s?", mayapFirmwareWebStatus().version);
    line1 = "CAP NHAT FIRMWARE";
    line2 = fwVerLine;
  } else if (currentRuntime.batchRunning) {
    line1 = "DUNG ME DANG CHAY?";
  } else {
    line1 = "BAT DAU ME AP MOI?";
  }

  const int16_t y1 = line2[0] ? 24 : 28;
  drawCenteredFit(y1, line1, u8g2_font_6x12_tf, u8g2_font_5x8_tf,
                  u8g2_font_5x8_tf);
  if (line2[0]) {
    drawCenteredFit(static_cast<int16_t>(y1 + 13), line2, u8g2_font_6x12_tf,
                    u8g2_font_5x8_tf, u8g2_font_5x8_tf);
  }

  drawYesNoButtons(confirmYes, "CO", "HUY");
}

void drawToast(uint32_t now) {
  if (!toastLine[0] || timeReached(now, toastUntil)) return;
  lcd.setDrawColor(1);
  lcd.drawBox(0, 55, 128, 9);
  lcd.setDrawColor(0);
  lcd.setFont(u8g2_font_5x8_tf);
  char line[32];
  snprintf(line, sizeof(line), "%s%s", toastError ? "! " : "", toastLine);
  // 5x8 la font nho nhat dang co nen khong the ha co them - cat bot duoi chuoi
  // cho vua 126px. showToast() da cat theo SO KY TU, nhung them tien to "! "
  // luc bao loi van co the day chuoi vuot qua be rong man hinh.
  while (line[0] && static_cast<int16_t>(lcd.getStrWidth(line)) > 126) {
    line[strlen(line) - 1U] = '\0';
  }
  const int16_t x = max(1, (128 - static_cast<int16_t>(lcd.getStrWidth(line))) / 2);
  lcd.drawStr(x, 63, line);
  lcd.setDrawColor(1);
}

bool probeLcdUnlocked();

void render(uint32_t now) {
  if (!lcdReady) return;
  const bool periodicHome = view == View::Home &&
                            now - lastHomeDrawAt >= HOME_REFRESH_MS;
  const bool periodicAlarm = view == View::Alarm &&
                             now - lastAlarmDrawAt >= ALARM_REFRESH_MS;
  const bool periodicFirmwareProgress = view == View::FirmwareProgress &&
      now - lastFirmwareProgressDrawAt >= FIRMWARE_PROGRESS_REFRESH_MS;

  // Man khoi dong: tu ve lai deu (khong ai set dirty ho) va tu thoat khi da
  // co du du lieu that. Xu ly TRUOC moi thu khac de khong bi cac view khac
  // (ke ca man canh bao/xac nhan) chen vao giua luc dang khoi dong.
  if (splashActive) {
    if (splashStartedAt == 0U) splashStartedAt = now;
    // The splash has NO timeout of its own. Home only when the boot coordinator releases it (valid stable sensor + every local safety
    // condition, or the operator pressed the button on a diagnostic screen). If the coordinator is stalled the HMI draws its own
    // E993 diagnostic; it never leaves the splash, and never shows "ready", on its own initiative.
    splashShown = MayapBoot::splashView(now, mayapBootHomeReleased(), mayapBootStatus() == MayapBoot::Status::Ready,
                                        mayapBootDiagnosticActive(), splashOperatorHome);
    if (splashShown == MayapBoot::SplashView::Home) {
      if (!mayapBootHomeReleased())
        mayapSerialPrintf(false, "[BOOT-HOME] operator override from E993 screen (coordinator stalled); heater stays gated by sensor/faults\n");
      splashActive = false;
      dirty = true;
    }
  }
  const bool periodicSplash = splashActive;
  // No prophylactic reinitialization or duplicate page draws. Static menus
  // stay still; only actual changes/dynamic data and real I2C faults redraw.
  const bool periodic = periodicHome || periodicAlarm ||
                        periodicSplash || periodicFirmwareProgress;
  if (!dirty && !periodic) return;
  if (now - lastDrawAt < DISPLAY_MIN_DRAW_MS) return;

  // Moi frame bat dau tu trang thai do hoa xac dinh. Khong tat/bat LCD khi
  // chuyen trang: framebuffer moi duoc ve trong RAM roi sendBuffer mot lan.
  lcd.setMaxClipWindow();
  lcd.setDrawColor(1);
  lcd.setFontMode(0);
  lcd.setFontDirection(0);
  lcd.clearBuffer();
  const bool showConfirmScreen = !splashActive && confirmationActive() &&
                                 view != View::Alarm;
  if (splashActive) {
    drawSplash();
  } else if (showConfirmScreen) {
    // Man xac nhan thay HAN cho view hien tai (khong ve chong len) - xem
    // drawConfirmScreen() de biet ly do doi tu dai 9px cuoi man sang ca man.
    drawConfirmScreen();
  } else {
    switch (view) {
      case View::Home: drawHome(); break;
      case View::MainMenu: drawMainMenu(); break;
      case View::ChungMenu: drawChungMenu(); break;
      case View::VentilationMenu: drawVentilationMenu(); break;
      case View::VentilationAdvanced: drawVentilationAdvanced(); break;
      case View::FirmwareMenu: drawFirmwareMenu(); break;
      case View::SettingList: drawSettingList(); break;
      case View::EditSetting: drawEditSetting(); break;
      case View::TurnStats: drawTurnStats(); break;
      case View::ConnectionInfo: drawConnectionInfo(); break;
      case View::QrCode: drawQrCode(); break;
      case View::AutoTune: drawAutoTune(); break;
      case View::TestMode: drawTestMode(); break;
      case View::TestSummary: drawTestSummary(); break;
      case View::WifiChange: drawWifiChange(); break;
      case View::FirmwareProgress: drawFirmwareProgress(); break;
      case View::EventLog: drawEventLog(); break;
      case View::Alarm: drawAlarm(); break;
      case View::TurnStatus: drawTurnStatus(); break;
    }
    // Man Canh bao dung dung dai 9px cuoi man (y~55-64) cho dong "E### x/y
    // NHAN=ACK" - day la thong tin BAT BUOC phai doc duoc (ma loi + cach xac
    // nhan). Toast ve DE LEN CUNG dai nay se che mat hoan toan trong 1.4-2.5s
    // moi lan toast xuat hien trong luc dang xem canh bao. Bo qua toast rieng
    // man nay - cac view khac deu chi mat thong tin PHU (trang thai chung,
    // dong cuoi 1 danh sach) trong chop mat, khong nghiem trong bang.
    if (view != View::Alarm) drawToast(now);
  }
  lcd.setDrawColor(1);
  if (i2cLockCallback && !i2cLockCallback(I2C_TIMEOUT_MS)) {
    dirty = true;
    return;
  }
  lcd.sendBuffer();
  // A missing panel cannot grant operational permission merely because the
  // void U8g2 sendBuffer() returned. Verify presence while holding the bus.
  const bool startupFrameOk = splashActive || mayapBootOperationsReady() ||
                              probeLcdUnlocked();
  if (i2cUnlockCallback) i2cUnlockCallback();
  if (!startupFrameOk) {
    lcdReady = false;
    lastLcdRetryAt = now;
    dirty = true;
    return;
  }
  if (!splashActive) mayapBootAcknowledgeHomeFrame();
  dirty = false;
  lastDrawAt = now;
  if (view == View::Home) lastHomeDrawAt = now;
  if (view == View::Alarm) lastAlarmDrawAt = now;
  if (view == View::FirmwareProgress) lastFirmwareProgressDrawAt = now;
}

// ============================================================
// 7. API HMI CHO FIRMWARE TONG
// ============================================================
bool probeLcdUnlocked() {
  Wire.beginTransmission(LCD_I2C_ADDRESS);
  const bool ok = Wire.endTransmission(true) == 0;
  mayapI2cReport(LCD_I2C_ADDRESS, ok);
  return ok;
}

// fullClear=true (mac dinh) dung lcd.begin() nguyen ban (initDisplay +
// clearDisplay + setPowerSave) - clearDisplay() ghi thang 1 khung TRANG/DEN
// xuong PANEL THAT qua I2C, tach biet hoan toan voi framebuffer RAM cua
// u8g2. Chi nen dung fullClear=true luc khoi dong lanh (chua co khung hinh
// dung nao dang hien, khong ai thay "flash"). Cac lan goi lai LCD khi may
// dang CHAY (health-check phuc hoi, tu lam moi dinh ky - xem serviceLcd())
// PHAI truyen fullClear=false: chi initDisplay()+setPowerSave() de nap lai
// chuoi lenh khoi tao ST7567 (sua thanh ghi noi bo bi nhieu lam sai), KHONG
// goi clearDisplay() - buoc nay thua vi ngay sau do dirty=true se ep ve lai
// TOAN BO khung That qua sendBuffer(). Bo qua clearDisplay() loai bo hoan
// toan 1 lan truyen I2C day khung trang (~vai chuc ms o 100kHz) dang gay
// "chop nhay man den 1 cai" ma nguoi dung thay khi thao tac (cuon nhanh hay
// binh thuong deu co the trung luc health-check/tu lam moi dinh ky chay).
bool beginLcd(bool fullClear = true) {
  if (i2cLockCallback && !i2cLockCallback(I2C_TIMEOUT_MS)) return false;
  Wire.setTimeOut(I2C_TIMEOUT_MS);
  if (!probeLcdUnlocked()) {
    if (i2cUnlockCallback) i2cUnlockCallback();
    return false;
  }
  lcd.setBusClock(I2C_CLOCK_HZ);
  lcd.setI2CAddress(static_cast<uint8_t>(LCD_I2C_ADDRESS << 1));
  if (fullClear) {
    lcd.begin();
  } else {
    lcd.initDisplay();
    lcd.setPowerSave(0);
  }
  lcd.setContrast(DEFAULT_CONTRAST);
  if (i2cUnlockCallback) i2cUnlockCallback();
  return true;
}

void serviceLcd(uint32_t now) {
  static uint32_t seenBusEpoch = 0U;
  const uint32_t busEpoch = mayapI2cRecoveryEpoch();
  if (busEpoch != seenBusEpoch) {
    seenBusEpoch = busEpoch;
    lcdReady = false;
    lastLcdRetryAt = now - LCD_RETRY_INTERVAL_MS;
    dirty = true;
  }
  if (!lcdReady) {
    if (now - lastLcdRetryAt < LCD_RETRY_INTERVAL_MS) return;
    lastLcdRetryAt = now;
    // fullClear=false: man dang hien noi dung CU (dung hoac da hong san,
    // khong co gi "tot" de giu) - bo qua clearDisplay() de tranh 1 khung
    // trang chen giua, dirty=true ben duoi se ve lai khung That ngay.
    lcdReady = beginLcd(false);
    if (lcdReady) {
      lastLcdHealthCheckAt = now;
      dirty = true;
      showToast("LCD DA TU PHUC HOI");
#if MAYAP_DIAGNOSTIC_SERIAL
      mayapSerialPrintf(false, "[HMI] LCD recovered\n");
#endif
    } else if (now - lastLcdFaultLogAt >= LCD_FAULT_LOG_INTERVAL_MS) {
      lastLcdFaultLogAt = now;
#if MAYAP_DIAGNOSTIC_SERIAL
      mayapSerialPrintf(false, "[HMI] LCD 0x%02X van mat\n", LCD_I2C_ADDRESS);
#endif
    }
    return;
  }

  if (now - lastLcdHealthCheckAt < LCD_HEALTH_CHECK_MS) return;
  lastLcdHealthCheckAt = now;
  if (i2cLockCallback && !i2cLockCallback(I2C_TIMEOUT_MS)) return;
  bool ok = probeLcdUnlocked();
  if (!ok) {
    // 1 xung nhieu chi anh huong 1 giao dich thoang qua thuong da het tac
    // dung chi sau vai mili giay - thu lai NGAY trong CHINH lan goi nay
    // (van giu nguyen khoa I2C xuyen suot, khong ai chen vao giua duoc)
    // thay vi doi den chu ky health-check SAU (LCD_HEALTH_CHECK_MS, 2s
    // nua) moi co diem du lieu thu 2 nhu truoc day - vua giam thoi gian
    // man hinh dung hinh khi that su co su co (xac nhan trong vai ms thay
    // vi toi 2s), vua van loc duoc da so nhieu don le trong tu cong
    // nghiep (chi 1 xung nhieu that su hiem khi lam sai CA HAI lan tham do
    // lien tiep sat nhau nhu vay).
    delay(2);
    ok = probeLcdUnlocked();
  }
  if (i2cUnlockCallback) i2cUnlockCallback();
  if (!ok) {
    lcdReady = false;
    lastLcdRetryAt = now;
    dirty = true;
#if MAYAP_DIAGNOSTIC_SERIAL
    mayapSerialPrintf(false, "[HMI] LCD/I2C lost, scheduling recovery\n");
#endif
  }
}

// Early display-only preview after safe outputs and I2C. No rotary/buzzer or
// runtime task is started until the formal HMI stage; ownership is then handed
// to hmiTask once, with no simultaneous LCD callers.
void hmiBootDisplayBegin() {
  lcdReady = beginLcd();
  splashStartedAt = millis();
  dirty = true;
}

void hmiBootDisplayUpdate(uint32_t now) {
  if (lcdReady) render(now);
}

void hmiBegin() {
  buzzerBegin();
  buzzerPlayStartupChime();
  beginRotary();
  if (!lcdReady) lcdReady = beginLcd();
  const uint32_t now = millis();
  lastInteractionAt = now;
  lastCommandPollAt = now;
  lastLcdRetryAt = now;
  lastLcdHealthCheckAt = now;
  // Moc thoi gian man khoi dong tinh tu luc BAT MAY, khong phai tu frame ve
  // dau tien: neu LCD chua nhan duoc luc khoi dong (dang tu do tim lai), den
  // khi no phuc hoi thi coordinator da nha Home (splash khong con han) va may vao thang man
  // chinh - khong bat nguoi dung xem lai man khoi dong giua chung.
  splashStartedAt = now;
  dirty = true;
#if MAYAP_DIAGNOSTIC_SERIAL
  mayapSerialPrintf(false, "[HMI] LCD=%s profile=%u contrast=%u\n", lcdReady ? "OK" : "FAIL",
                   LCD_PROFILE, DEFAULT_CONTRAST);
#endif
}

void sanitizeRuntime(MachineRuntime &runtime) {
  runtime.dateText[sizeof(runtime.dateText) - 1U] = '\0';
  runtime.machineState[sizeof(runtime.machineState) - 1U] = '\0';
  runtime.alarmMask &= ALARM_KNOWN_MASK;
  if (!isfinite(runtime.temperature) || !isfinite(runtime.humidity)) {
    runtime.sensorOnline = false;
  }
  if (!isfinite(runtime.temperature)) runtime.temperature = 0.0f;
  if (!isfinite(runtime.humidity)) runtime.humidity = 0.0f;
  runtime.temperature = constrain(runtime.temperature, -40.0f, 125.0f);
  runtime.humidity = constrain(runtime.humidity, 0.0f, 100.0f);
  if (!isfinite(runtime.heaterPower)) runtime.heaterPower = 0.0f;
  runtime.heaterPower = constrain(runtime.heaterPower, 0.0f, 100.0f);
  if (static_cast<uint8_t>(runtime.turnState) >
      static_cast<uint8_t>(TurnState::Fault)) {
    runtime.turnState = TurnState::Fault;
  }
  if (static_cast<uint8_t>(runtime.autoTuneState) >
      static_cast<uint8_t>(AutoTuneState::Failed)) {
    runtime.autoTuneState = AutoTuneState::Failed;
  }
  if (static_cast<uint8_t>(runtime.connectivityMode) >
      static_cast<uint8_t>(ConnectivityMode::Online)) {
    runtime.connectivityMode = ConnectivityMode::Offline;
  }
  if (static_cast<uint8_t>(runtime.networkState) >
      static_cast<uint8_t>(NetworkStateCode::Connected)) {
    runtime.networkState = NetworkStateCode::Offline;
  }
  if (runtime.networkRssiDbm < -127) runtime.networkRssiDbm = -127;
  if (runtime.networkRssiDbm > 0) runtime.networkRssiDbm = 0;
  runtime.networkConnected =
      runtime.connectivityMode == ConnectivityMode::Online &&
      runtime.networkConfigured &&
      runtime.networkState == NetworkStateCode::Connected &&
      runtime.networkConnected;
  runtime.autoTuneProgress = static_cast<uint8_t>(constrain(
      static_cast<int>(runtime.autoTuneProgress), 0, 100));
  if (runtime.activeFaultDisplayCount > HMI_FAULT_DISPLAY_CAPACITY) {
    runtime.activeFaultDisplayCount = HMI_FAULT_DISPLAY_CAPACITY;
  }
  if (runtime.activeFaultCount < runtime.activeFaultDisplayCount) {
    runtime.activeFaultCount = runtime.activeFaultDisplayCount;
  }
  if (runtime.autoTuneState == AutoTuneState::Success) {
    runtime.autoTuneProgress = 100;
  } else if (runtime.autoTuneState == AutoTuneState::Idle ||
             runtime.autoTuneState == AutoTuneState::Failed) {
    runtime.autoTuneProgress = 0;
  }
  if (!runtime.dateText[0]) snprintf(runtime.dateText, sizeof(runtime.dateText),
                                     "--/--/----");
  if (!runtime.machineState[0]) {
    snprintf(runtime.machineState, sizeof(runtime.machineState),
             runtime.batchRunning ? "DANG AP" : "SAN SANG");
  }
}

int16_t displayTenths(float value) {
  return static_cast<int16_t>(lroundf(value * 10.0f));
}

int16_t displayWhole(float value) {
  return static_cast<int16_t>(lroundf(value));
}

bool fixedTextChanged(const char *a, const char *b, size_t size) {
  return strncmp(a, b, size) != 0;
}

bool runtimeHeaderChanged(const MachineRuntime &before,
                          const MachineRuntime &after) {
  return before.alarmMask != after.alarmMask ||
         before.primaryFaultCode != after.primaryFaultCode ||
         before.activeFaultCount != after.activeFaultCount ||
         before.faultNotificationSequence != after.faultNotificationSequence ||
         fixedTextChanged(before.dateText, after.dateText,
                          sizeof(before.dateText));
}

// Chi danh dau ve lai khi du lieu dang nhin thay tren trang hien tai thay doi.
// Chi danh dau dirty khi noi dung dang hien thi thuc su thay doi; khong
// lam LCD gui lai 1024 byte neu trang hien tai khong hien chung.
bool runtimeVisibleChanged(const MachineRuntime &before,
                           const MachineRuntime &after) {
  if (runtimeHeaderChanged(before, after)) return true;

  switch (view) {
    case View::Home:
      if (homePage == 0) {
        return before.sensorOnline != after.sensorOnline ||
               (after.sensorOnline &&
                displayTenths(before.temperature) !=
                    displayTenths(after.temperature)) ||
               (after.sensorOnline &&
                displayWhole(before.humidity) != displayWhole(after.humidity)) ||
               before.batchRunning != after.batchRunning ||
               before.currentDay != after.currentDay ||
               before.nextTurnMinutes != after.nextTurnMinutes ||
               before.nextTurnScheduled != after.nextTurnScheduled ||
               before.turningLockdown != after.turningLockdown ||
               before.connectivityMode != after.connectivityMode ||
               before.networkConfigured != after.networkConfigured ||
               before.networkConnected != after.networkConnected ||
               before.networkRssiDbm != after.networkRssiDbm ||
               before.wifiPortalState != after.wifiPortalState ||
               fixedTextChanged(before.machineState, after.machineState,
                                sizeof(before.machineState));
      }
      return before.heaterOn != after.heaterOn ||
             displayWhole(before.heaterPower) != displayWhole(after.heaterPower) ||
             before.circulationFanOn != after.circulationFanOn ||
             before.ventFanOn != after.ventFanOn ||
             before.turnState != after.turnState ||
             before.turnHoming != after.turnHoming ||
             before.turningLockdown != after.turningLockdown ||
             before.nextTurnMinutes != after.nextTurnMinutes ||
             before.nextTurnScheduled != after.nextTurnScheduled;

    case View::TurnStats:
      return before.turnCountToday != after.turnCountToday ||
             before.turnCountBatch != after.turnCountBatch ||
             before.nextTurnMinutes != after.nextTurnMinutes ||
             before.nextTurnScheduled != after.nextTurnScheduled ||
             before.turningLockdown != after.turningLockdown;

    case View::ConnectionInfo:
      return before.networkConnected != after.networkConnected ||
             before.networkRssiDbm != after.networkRssiDbm;

    // QR ma hoa ID may - hang so trong suot phien chay, khong bao gio can
    // ve lai vi runtime thay doi.
    case View::QrCode:
      return false;

    case View::AutoTune:
      return before.autoTuneState != after.autoTuneState ||
             before.autoTuneProgress != after.autoTuneProgress ||
             before.batchRunning != after.batchRunning ||
             before.sensorOnline != after.sensorOnline;

    case View::EventLog:
      return before.eventSequence != after.eventSequence;

    case View::Alarm:
      return displayTenths(before.temperature) !=
                 displayTenths(after.temperature) ||
             displayWhole(before.humidity) != displayWhole(after.humidity) ||
             before.primaryFaultCode != after.primaryFaultCode ||
             before.activeFaultDisplayCount != after.activeFaultDisplayCount ||
             before.faultNotificationSequence != after.faultNotificationSequence;

    case View::TurnStatus:
      return before.turnState != after.turnState ||
             before.turnHoming != after.turnHoming;

    case View::TestMode:
      return before.testModeActive != after.testModeActive ||
             before.testOutputMaskActive != after.testOutputMaskActive ||
             before.testLimitTarget != after.testLimitTarget ||
             before.testLimitPhase != after.testLimitPhase;

    case View::WifiChange:
      return before.wifiPortalState != after.wifiPortalState ||
             fixedTextChanged(before.wifiPortalApName, after.wifiPortalApName,
                              sizeof(before.wifiPortalApName));

    case View::MainMenu:
    case View::ChungMenu:
    case View::SettingList:
    case View::EditSetting:
    case View::TestSummary:
      return false;
  }
  return false;
}

void applyRuntime(MachineRuntime runtime) {
  sanitizeRuntime(runtime);
  const bool wifiJustLost =
      currentRuntime.connectivityMode == ConnectivityMode::Online &&
      currentRuntime.networkConfigured && currentRuntime.networkConnected &&
      runtime.connectivityMode == ConnectivityMode::Online &&
      runtime.networkConfigured && !runtime.networkConnected &&
      runtime.wifiPortalState == WifiPortalState::Idle;
  const bool wifiJustRecovered =
      currentRuntime.connectivityMode == ConnectivityMode::Online &&
      currentRuntime.networkConfigured && !currentRuntime.networkConnected &&
      runtime.connectivityMode == ConnectivityMode::Online &&
      runtime.networkConfigured && runtime.networkConnected;
  const AutoTuneState previousAutoTuneState = currentRuntime.autoTuneState;
  const bool newFaultOccurrence =
      runtime.faultNotificationSequence != currentRuntime.faultNotificationSequence &&
      runtime.lastRaisedFaultCode != 0U;
  const uint32_t newFaultAlarmBit = newFaultOccurrence
      ? alarmBitForFaultCode(runtime.lastRaisedFaultCode) : AlarmNone;
  const uint32_t newAlarmBits = runtime.alarmMask & ~alarmPresentedMask;
  alarmPresentedMask &= runtime.alarmMask;
  // Canh (edge) dong co VUA bat dau chay dao/tim goc - tuc la truoc do KHONG
  // phai Left/Right, gio thanh Left/Right. Dung de tu bat man phu TurnStatus
  // dung 1 lan moi dot dao/tim goc, giong het newFaultOccurrence o tren.
  const bool newTurnMoveOccurrence =
      (runtime.turnState == TurnState::Left ||
       runtime.turnState == TurnState::Right) &&
      currentRuntime.turnState != TurnState::Left &&
      currentRuntime.turnState != TurnState::Right;

  const bool visibleChange = runtimeVisibleChanged(currentRuntime, runtime);
  currentRuntime = runtime;
  if (wifiJustLost) {
    wifiOfflineTracking = true;
    wifiOfflineSinceAt = millis();
    wifiOfflineNoticeActive = true;
    showToast("MAT WIFI - MAY VAN CHAY", true, 8000UL);
  } else if (wifiJustRecovered) {
    wifiOfflineTracking = false;
    wifiOfflineNoticeActive = false;
    wifiOfflineSinceAt = 0U;
    showToast("WIFI DA KET NOI LAI");
  }
  buzzer.acknowledgedAlarmMask &= currentRuntime.alarmMask;
  // Moi ma loi moi deu duoc keu lai, ke ca no dung chung AlarmBit voi mot loi
  // cu da duoc nguoi dung ACK truoc do.
  if (newFaultAlarmBit != AlarmNone) buzzerUnacknowledge(newFaultAlarmBit);

  if (currentRuntime.autoTuneState != previousAutoTuneState) {
    if (currentRuntime.autoTuneState == AutoTuneState::Running) {
      showToast("AUTO TUNE DA BAT DAU");
    } else if (currentRuntime.autoTuneState == AutoTuneState::Success) {
      showToast("AUTO TUNE THANH CONG");
      buzzerPlayCue(BuzzerCue::Ok);
    } else if (currentRuntime.autoTuneState == AutoTuneState::Failed) {
      showToast("AUTO TUNE THAT BAI", true);
      buzzerPlayCue(BuzzerCue::Error);
    }
  }

  if (!currentRuntime.resumeConfirmationRequired) resumeDecisionSubmitted = false;
  if (!currentRuntime.batchOverdueConfirmationPending) batchOverdueDecisionSubmitted = false;

  // Ket qua kiem tra cong tac hanh trinh la khach quan (phan cung tu phat
  // hien), khac voi thiet bi (nguoi dung tu xac nhan). Ghi lai khi vua co
  // ket qua moi de hien trong bang tong ket.
  if (currentRuntime.testLimitPhase != lastObservedTestLimitPhase) {
    if (currentRuntime.testLimitPhase == TestLimitPhase::Success ||
        currentRuntime.testLimitPhase == TestLimitPhase::Timeout) {
      const uint8_t idx = static_cast<uint8_t>(currentRuntime.testLimitTarget);
      if (idx < TEST_MODE_LIMIT_ROWS) {
        testLimitResult[idx] = currentRuntime.testLimitPhase == TestLimitPhase::Success
            ? TestResult::Pass : TestResult::Fail;
      }
      // Xac nhan am thanh bang coi NOI BO cua HMI, khong phai relay coi that
      // (truoc day dung req.siren ben firmware - da sua vi lam keu coi that
      // ngoai y muon moi lan test cong tac hanh trinh, xem machine_control.h).
      buzzerPlayCue(currentRuntime.testLimitPhase == TestLimitPhase::Success
          ? BuzzerCue::Ok : BuzzerCue::Error);
    }
    lastObservedTestLimitPhase = currentRuntime.testLimitPhase;
  }

  // Nguoi dung dang thao tac CHU DONG o noi khac (menu cai dat, man xac
  // nhan Bat/Dung me, nhat ky su kien...) khong nen bi "giat" ve man Canh
  // bao giua chung chi vi mot loi CANH BAO (Warning - may van ap binh
  // thuong, vd do am/nhiet do dao dong quanh nguong) VUA TAI KICH HOAT -
  // alarmPresentedMask bi xoa bit ngay khi loi tam het active (dong 4121 o
  // tren) nen lan tai kich hoat ke tiep cua CHINH loi do van bi tinh la
  // "moi" (newFaultOccurrence) du nguoi dung da thay no roi. Truoc day
  // truong hop nay luon ngat ngang bat ke dang lam gi, khien viec nhan giu
  // vao Menu cai dat hay xac nhan Ket thuc me tro nen rat kho moi khi dang
  // co canh bao (bi vang man hinh moi vai giay). Loi Stop/Emergency (may
  // KHONG con ap duoc binh thuong nua) va bat ky AlarmBit HOAN TOAN chua
  // tung hien (newAlarmBits) van LUON duoc uu tien hien NGAY bat ke dang
  // lam gi, vi day la thong tin an toan nguoi dung phai biet ngay lap tuc.
  const bool userBusyElsewhere = view == View::MainMenu ||
      view == View::ChungMenu || view == View::SettingList ||
      view == View::EditSetting || view == View::TestMode ||
      view == View::TestSummary || view == View::EventLog ||
      confirmationActive();
  const bool faultSeverityCritical =
      runtime.lastRaisedFaultSeverity >= FAULT_SEVERITY_STOP_THRESHOLD;
  const bool shouldShowAlarmNow = newAlarmBits ||
      (newFaultOccurrence && (faultSeverityCritical || !userBusyElsewhere));

  if (shouldShowAlarmNow) {
    alarmPresentedMask |= newAlarmBits | newFaultAlarmBit;
    if (view != View::Alarm) alarmReturnView = view;
    alarmIndex = 0;
    view = View::Alarm;
    dirty = true;
  } else if (newFaultOccurrence) {
    // Van danh dau la "da trinh dien" du khong doi man hinh, tranh lan tai
    // kich hoat KE TIEP cua chinh loi nay bi hieu nham la moi mai. "!" o dau
    // man hinh (drawHeader) va coi/buzzer van bao binh thuong nhu cu - chi
    // khac o cho khong chen ngang man hinh nguoi dung dang dung thao tac.
    alarmPresentedMask |= newFaultAlarmBit;
  } else if (!runtime.alarmMask && view == View::Alarm) {
    view = alarmReturnView;
    dirty = true;
  }

  // Man phu TurnStatus (dang dao/tim goc) - co che giong het Alarm o tren:
  // tu bat khi CO SU KIEN MOI (canh len cua turnState->Left/Right), tu tat
  // khi dong co ngung chay. KHONG duoc chen ngang man Alarm (loi luon uu
  // tien hien thi) - da co loi thi bo qua, cho Alarm tu dong dong truoc.
  const bool turnStatusMoving = currentRuntime.turnState == TurnState::Left ||
                                 currentRuntime.turnState == TurnState::Right;
  if (newTurnMoveOccurrence && !currentRuntime.activeFaultDisplayCount &&
      view != View::Alarm) {
    if (view != View::TurnStatus) turnStatusReturnView = view;
    view = View::TurnStatus;
    dirty = true;
  } else if (!turnStatusMoving && view == View::TurnStatus) {
    view = turnStatusReturnView;
    dirty = true;
  }

  // Sau khi xu ly man hinh loi, neu day la khoi dong lai sau mat dien thi
  // luon dua nguoi dung den man hinh xac nhan. Khong de trang Alarm cu che
  // mat yeu cau tiep tuc/huy me.
  if (currentRuntime.resumeConfirmationRequired && !resumeDecisionSubmitted &&
      confirmAction != ConfirmAction::ResumeBatch && view != View::Alarm &&
      view != View::TurnStatus) {
    openResumeConfirm();
  }

  // F-06: me qua han ngay du kien - dua nguoi dung den man hinh xac nhan
  // "tiep tuc u am?" giong het cach lam voi man hinh phuc hoi sau mat dien
  // o tren (nhung uu tien thap hon: chi bat khi khong co man hinh xac nhan
  // nao khac dang mo).
  if (currentRuntime.batchOverdueConfirmationPending && !batchOverdueDecisionSubmitted &&
      confirmAction == ConfirmAction::None && view != View::Alarm &&
      view != View::TurnStatus) {
    openBatchOverdueContinueConfirm();
  }

  // Nhac dinh ky "co ban firmware moi" - CHI bat len khi dang o man hinh
  // chinh, ranh (khong co viec gi khac dang cho xac nhan/canh bao), tranh
  // ngat ngang luc nguoi dung dang thao tac o menu khac. Bam Co/Khong xem
  // executeConfirmation() (ConfirmAction::FirmwareWebApply).
  if (mayapFirmwareWebStatus().available && view == View::Home &&
      confirmAction == ConfirmAction::None &&
      (firmwareReminderLastAt == 0U ||
       (millis() - firmwareReminderLastAt) >= FIRMWARE_REMINDER_INTERVAL_MS)) {
    openFirmwareWebConfirm();
  }
  if (visibleChange) dirty = true;
}

void applyHostConfig(MachineConfig config) {
  sanitizeConfig(config);
  // Dong bo tu firmware khong duoc tu y day nguoi dung ra khoi trang dang mo.
  // ACK luu se xu ly transaction; mailbox config chi cap nhat gia tri nen.
  currentConfig = config;
  if (view == View::EditSetting && !configSave.active) {
    const SettingItem &item = SETTINGS[editSettingIndex];
    float minimum, maximum;
    settingLimits(currentConfig, item, minimum, maximum);
    editValue = constrain(editValue, minimum, maximum);
  } else if (view == View::SettingList) {
    // groupVisibleExtraCount() (vd Doi wifi/Dat lai PIN/Cap nhat firmware)
    // phu thuoc currentConfig.connectivityMode - config nay vua doi phia
    // tren nen so dong THAT SU hien ra co the vua co/mat di ngay tuc khac.
    // Khong re-clamp o day thi listIndex/listTop co the tro vao 1 dong vua
    // bien mat (khong con gi duoc to sang) hoac vuot qua "Thoat" cho toi khi
    // nguoi dung tu xoay num lai (setListSelection() moi tu clamp).
    setListSelection(listIndex, settingListItemCount(selectedGroup));
  }
  dirty = true;
}

void processConfigAck(const ConfigAckInbox &ack) {
  MachineConfig rollback;
  MachineConfig accepted;
  bool matched = false;
  portENTER_CRITICAL(&hmiApiMux);
  if (configSave.active && configSave.id == ack.transactionId) {
    rollback = configSave.rollback;
    accepted = ack.hasStoredConfig ? ack.storedConfig : configSave.candidate;
    configSave.active = false;
    configSave.readyForHost = false;
    matched = true;
  }
  portEXIT_CRITICAL(&hmiApiMux);
  if (!matched) return;

  if (ack.ok) {
    sanitizeConfig(accepted);
    currentConfig = accepted;
    showToast("DA LUU CAU HINH");
    buzzerPlayCue(BuzzerCue::Save);
  } else {
    currentConfig = rollback;
    showToast("LOI LUU - DA HOAN TAC", true);
    buzzerPlayCue(BuzzerCue::Error);
  }
  dirty = true;
}

void processCommandAcks() {
  for (uint8_t budget = 0U; budget < COMMAND_ACK_QUEUE_SIZE; ++budget) {
    CommandAck ack;
    HmiCommand command;
    bool haveAck = false;
    bool matched = false;
    portENTER_CRITICAL(&hmiApiMux);
    if (commandAckCount) {
      ack = commandAckQueue[commandAckHead];
      commandAckHead = static_cast<uint8_t>(
          (commandAckHead + 1U) % COMMAND_ACK_QUEUE_SIZE);
      --commandAckCount;
      haveAck = true;
      for (uint8_t i = 0; i < COMMAND_QUEUE_SIZE; ++i) {
        if (activeCommands[i].used &&
            activeCommands[i].command.id == ack.commandId) {
          command = activeCommands[i].command;
          activeCommands[i].used = false;
          if (commandOutstandingCount) --commandOutstandingCount;
          matched = true;
          break;
        }
      }
    }
    portEXIT_CRITICAL(&hmiApiMux);
    if (!haveAck) break;
    if (!matched) continue;

    if (command.type == HmiCommandType::AlarmAck) {
      if (!ack.ok) buzzerUnacknowledge(command.alarmMask);
    } else {
      buzzerPlayCue(ack.ok ? BuzzerCue::Ok : BuzzerCue::Error);
    }
    showToast(ack.message[0] ? ack.message :
              (ack.ok ? "LENH DA THUC HIEN" : "LENH BI TU CHOI"), !ack.ok);

    // Neu yeu cau mo cong Wi-Fi/vao thu nghiem bi tu choi ngay tai firmware
    // tong, dua nguoi dung tro lai man hinh truoc do thay vi ket ket qua cho.
    if (!ack.ok && command.type == HmiCommandType::WifiPortalStart &&
        view == View::WifiChange) {
      goBack();
    }
    if (!ack.ok && command.type == HmiCommandType::FirmwareWebApply &&
        view == View::FirmwareProgress) {
      view = View::FirmwareMenu;
      dirty = true;
    }
    if (!ack.ok && command.type == HmiCommandType::TestOutputPulse &&
        command.alarmMask == static_cast<uint32_t>(TestOutputId::HeaterSsr)) {
      heaterTestUiPhase = HeaterTestUiPhase::Idle;
      dirty = true;
    }
    if (!ack.ok && command.type == HmiCommandType::TestModeEnter &&
        view == View::TestMode) {
      view = View::ChungMenu;
      chungIndex = chungTestIndex();  // "CHE DO TEST"
      alignChungMenuWindow();
      dirty = true;
    }
  }
}

void serviceApiMailboxes() {
  // hmiUpdate() co the duoc goi rat nhanh. Khong vao critical section neu
  // firmware tong khong gui du lieu/ACK moi.
  if (!apiHasPendingWork()) return;

  // Scratch tinh: HMI chi cho phep mot task goi hmiUpdate(), tranh khoi tao
  // lai MachineConfig/MachineRuntime lon tren stack o moi vong loop.
  static MachineRuntime runtime;
  static MachineConfig config;
  static HmiEventSnapshot eventLog;
  static ConfigAckInbox configAck;
  static char date[11];
  bool hasRuntime = false;
  bool hasConfig = false;
  bool hasEventLog = false;
  bool hasConfigAck = false;
  bool hasDate = false;
  bool hasCue = false;
  BuzzerCue cue = BuzzerCue::None;

  portENTER_CRITICAL(&hmiApiMux);
  if (runtimeInboxPending) {
    runtime = runtimeInbox;
    runtimeInboxPending = false;
    hasRuntime = true;
  }
  if (configInboxPending) {
    config = configInbox;
    configInboxPending = false;
    hasConfig = true;
  }
  if (eventLogInboxPending) {
    eventLog = eventLogInbox;
    eventLogInboxPending = false;
    hasEventLog = true;
  }
  if (configAckInbox.pending) {
    configAck = configAckInbox;
    configAckInbox.pending = false;
    hasConfigAck = true;
  }
  if (dateInboxPending) {
    memcpy(date, dateInbox, sizeof(date));
    dateInboxPending = false;
    hasDate = true;
  }
  if (cueInboxPending) {
    cue = cueInbox;
    cueInboxPending = false;
    hasCue = true;
  }
  portEXIT_CRITICAL(&hmiApiMux);

  if (hasConfigAck) processConfigAck(configAck);
  if (hasConfig) applyHostConfig(config);
  if (hasRuntime) applyRuntime(runtime);
  // Danh dau da co so lieu that de man khoi dong biet luc nao duoc thoat
  // (xem splashActive trong render()) - chi can moi loai den it nhat 1 lan.
  if (hasConfig) splashHadConfig = true;
  if (hasRuntime) splashHadRuntime = true;
  if (hasEventLog) {
    currentEventLog = eventLog;
    rememberRecentFaults(eventLog, millis());
    if (eventLogIndex >= eventLogVisibleCount()) eventLogIndex = 0U;
    if (view == View::EventLog) dirty = true;
  }
  if (hasCue) buzzerPlayCue(cue);
  if (hasDate &&
      strncmp(date, currentRuntime.dateText, sizeof(currentRuntime.dateText)) != 0) {
    memcpy(currentRuntime.dateText, date, sizeof(currentRuntime.dateText));
    currentRuntime.dateText[sizeof(currentRuntime.dateText) - 1U] = '\0';
    dirty = true;
  }
  processCommandAcks();

  portENTER_CRITICAL(&hmiApiMux);
  const bool stillPending = runtimeInboxPending || configInboxPending ||
                            eventLogInboxPending || configAckInbox.pending ||
                            dateInboxPending || cueInboxPending ||
                            commandAckCount != 0;
  setApiWorkPending(stillPending);
  portEXIT_CRITICAL(&hmiApiMux);
}

void serviceCommandTimeouts(uint32_t now) {
  uint32_t alarmMaskToUnack = AlarmNone;
  bool anyTimeout = false;
  portENTER_CRITICAL(&hmiApiMux);

  HmiCommand retained[COMMAND_QUEUE_SIZE];
  uint8_t retainedCount = 0;
  while (commandCount) {
    const HmiCommand command = commandQueue[commandHead];
    commandHead = static_cast<uint8_t>((commandHead + 1U) % COMMAND_QUEUE_SIZE);
    --commandCount;
    if (timeReached(now, command.createdAt) &&
        now - command.createdAt >= command.validForMs) {
      if (commandOutstandingCount) --commandOutstandingCount;
      if (command.type == HmiCommandType::AlarmAck) {
        alarmMaskToUnack |= command.alarmMask;
      }
      anyTimeout = true;
    } else {
      retained[retainedCount++] = command;
    }
  }
  commandHead = 0;
  commandTail = retainedCount % COMMAND_QUEUE_SIZE;
  commandCount = retainedCount;
  for (uint8_t i = 0; i < retainedCount; ++i) commandQueue[i] = retained[i];

  for (uint8_t i = 0; i < COMMAND_QUEUE_SIZE; ++i) {
    if (!activeCommands[i].used) continue;
    if (now - activeCommands[i].takenAt < COMMAND_CONFIRM_TIMEOUT_MS) {
      continue;
    }
    if (activeCommands[i].command.type == HmiCommandType::AlarmAck) {
      alarmMaskToUnack |= activeCommands[i].command.alarmMask;
    }
    activeCommands[i].used = false;
    if (commandOutstandingCount) --commandOutstandingCount;
    anyTimeout = true;
  }
  if (expiredAlarmAckMask) anyTimeout = true;
  alarmMaskToUnack |= expiredAlarmAckMask;
  expiredAlarmAckMask = AlarmNone;
  portEXIT_CRITICAL(&hmiApiMux);

  if (alarmMaskToUnack) buzzerUnacknowledge(alarmMaskToUnack);
  if (anyTimeout) {
    showToast("LENH QUA THOI GIAN", true);
    buzzerPlayCue(BuzzerCue::Error);
  }
}

void serviceConfigSaveTimeout(uint32_t now) {
  MachineConfig rollback;
  bool timedOut = false;
  portENTER_CRITICAL(&hmiApiMux);
  if (configSave.active && timeReached(now, configSave.startedAt) &&
      now - configSave.startedAt >= SAVE_CONFIRM_TIMEOUT_MS) {
    rollback = configSave.rollback;
    configSave.active = false;
    configSave.readyForHost = false;
    timedOut = true;
  }
  portEXIT_CRITICAL(&hmiApiMux);
  if (!timedOut) return;
  currentConfig = rollback;
  showToast("LUU QUA THOI GIAN - DA HOAN TAC", true);
  buzzerPlayCue(BuzzerCue::Error);
}

void hmiUpdate(uint32_t now) {
  serviceApiMailboxes();
  serviceOnlineHealthNotice(now);
  serviceHeaterTestWorkflow(now);
  stabilizeViewTransition();
  if (now - lastCommandPollAt >= HMI_COMMAND_POLL_MS) {
    lastCommandPollAt = now;
    serviceCommandTimeouts(now);
    serviceConfigSaveTimeout(now);
  }

  updateRotary(now);
  if (rotary.step || rotary.button != ButtonEvent::None) {
    lastInteractionAt = now;
  }
  handleInput();
  stabilizeViewTransition();

  if (!confirmationActive() && heaterTestUiPhase == HeaterTestUiPhase::Idle &&
      view != View::Home && view != View::Alarm &&
      now - lastInteractionAt >=
          ((view == View::WifiChange) ? WIFI_PORTAL_UI_IDLE_TIMEOUT_MS
                                      : MENU_IDLE_TIMEOUT_MS)) {
    // Roi Che do thu nghiem/Doi Wi-Fi do khong thao tac phai dong hang han
    // ngay tren firmware tong, khong chi tam roi man hinh.
    if (view == View::TestMode) queueCommand(HmiCommandType::TestModeExit);
    else if (view == View::WifiChange) queueCommand(HmiCommandType::WifiPortalCancel);
    view = View::Home;
    homePage = 0;
    showToast("TU DONG VE MAN HINH CHINH");
    lastInteractionAt = now;
  }

  buzzerUpdate(now);
  if (toastLine[0]) {
    if (confirmationActive()) {
      // Man xac nhan CO/KHONG dang che dai toast (drawToast() khong ve gi
      // luc showConfirmScreen=true, xem render()) - va khong co han tu dong
      // dong man xac nhan (MENU_IDLE_TIMEOUT_MS chu dong loai tru truong hop
      // nay). Neu cu de toastUntil troi qua binh thuong trong luc bi che, no
      // se bi clearToast() xoa truoc khi nguoi dung tung thay - vd ACK 1
      // canh bao roi giu nut mo hoi thoai "DUNG KHAN CAP" ngay sau do, ket
      // qua ACK/timeout lenh khong bao gio hien ra. Doi han moi chu ky de
      // "tam dung dong ho" - toast luon con nguyen 1 khoang hien thi day du
      // ngay khi man xac nhan dong lai.
      toastUntil = now + (toastError ? TOAST_ERROR_MS : TOAST_INFO_MS);
    } else if (timeReached(now, toastUntil)) {
      clearToast();
      dirty = true;
    }
  }
  serviceLcd(now);
  render(now);
}

void hmiSetConfig(const MachineConfig &config) {
  portENTER_CRITICAL(&hmiApiMux);
  configInbox = config;
  configInboxPending = true;
  markApiWorkPending();
  portEXIT_CRITICAL(&hmiApiMux);
}

void hmiSetDate(const char *dateText) {
  if (!dateText || !dateText[0]) dateText = "--/--/----";
  portENTER_CRITICAL(&hmiApiMux);
  snprintf(dateInbox, sizeof(dateInbox), "%.10s", dateText);
  dateInboxPending = true;
  markApiWorkPending();
  portEXIT_CRITICAL(&hmiApiMux);
}

const MachineConfig &hmiGetConfig() { return currentConfig; }

void hmiSetRuntime(const MachineRuntime &runtime) {
  portENTER_CRITICAL(&hmiApiMux);
  const uint32_t accumulatedAlarmMask =
      runtimeInboxPending ? runtimeInbox.alarmMask : AlarmNone;
  runtimeInbox = runtime;
  runtimeInbox.alarmMask |= accumulatedAlarmMask;
  runtimeInboxPending = true;
  markApiWorkPending();
  portEXIT_CRITICAL(&hmiApiMux);
}

void hmiSetEventLog(const HmiEventSnapshot &snapshot) {
  portENTER_CRITICAL(&hmiApiMux);
  eventLogInbox = snapshot;
  eventLogInboxPending = true;
  markApiWorkPending();
  portEXIT_CRITICAL(&hmiApiMux);
}

bool hmiTakeSavedConfig(MachineConfig &out, uint32_t &transactionId) {
  bool available = false;
  portENTER_CRITICAL(&hmiApiMux);
  if (configSave.active && configSave.readyForHost) {
    out = configSave.candidate;
    transactionId = configSave.id;
    configSave.readyForHost = false;
    available = true;
  }
  portEXIT_CRITICAL(&hmiApiMux);
  return available;
}

bool hmiConfirmConfigSave(uint32_t transactionId, bool ok,
                          const MachineConfig *storedConfig) {
  bool accepted = false;
  portENTER_CRITICAL(&hmiApiMux);
  if (!configAckInbox.pending && configSave.active &&
      configSave.id == transactionId) {
    configAckInbox.pending = true;
    configAckInbox.transactionId = transactionId;
    configAckInbox.ok = ok;
    configAckInbox.hasStoredConfig = storedConfig != nullptr;
    if (storedConfig) configAckInbox.storedConfig = *storedConfig;
    markApiWorkPending();
    accepted = true;
  }
  portEXIT_CRITICAL(&hmiApiMux);
  return accepted;
}



bool hmiTakeCommand(HmiCommand &out) {
  const uint32_t now = millis();
  bool available = false;
  portENTER_CRITICAL(&hmiApiMux);
  while (commandCount && !available) {
    const HmiCommand command = commandQueue[commandHead];
    commandHead = static_cast<uint8_t>((commandHead + 1U) % COMMAND_QUEUE_SIZE);
    --commandCount;
    if (timeReached(now, command.createdAt) &&
        now - command.createdAt >= command.validForMs) {
      if (commandOutstandingCount) --commandOutstandingCount;
      if (command.type == HmiCommandType::AlarmAck) {
        expiredAlarmAckMask |= command.alarmMask;
      }
      continue;
    }
    for (uint8_t i = 0; i < COMMAND_QUEUE_SIZE; ++i) {
      if (activeCommands[i].used) continue;
      activeCommands[i].used = true;
      activeCommands[i].takenAt = now;
      activeCommands[i].command = command;
      out = command;
      available = true;
      break;
    }
    if (!available) {
      if (commandOutstandingCount) --commandOutstandingCount;
      if (command.type == HmiCommandType::AlarmAck) {
        expiredAlarmAckMask |= command.alarmMask;
      }
    }
  }
  portEXIT_CRITICAL(&hmiApiMux);
  return available;
}

bool hmiConfirmCommand(uint32_t commandId, bool ok, const char *message) {
  bool exists = false;
  bool accepted = false;
  portENTER_CRITICAL(&hmiApiMux);
  for (uint8_t i = 0; i < COMMAND_QUEUE_SIZE; ++i) {
    if (activeCommands[i].used &&
        activeCommands[i].command.id == commandId) {
      exists = true;
      break;
    }
  }
  if (exists && commandAckCount < COMMAND_ACK_QUEUE_SIZE) {
    CommandAck &ack = commandAckQueue[commandAckTail];
    ack.commandId = commandId;
    ack.ok = ok;
    snprintf(ack.message, sizeof(ack.message), "%s", message ? message : "");
    commandAckTail = static_cast<uint8_t>(
        (commandAckTail + 1U) % COMMAND_ACK_QUEUE_SIZE);
    ++commandAckCount;
    markApiWorkPending();
    accepted = true;
  }
  portEXIT_CRITICAL(&hmiApiMux);
  return accepted;
}

void hmiSetI2cLockCallbacks(HmiI2cLockFn lockFn,
                            HmiI2cUnlockFn unlockFn) {
  portENTER_CRITICAL(&hmiApiMux);
  if (lockFn && unlockFn) {
    i2cLockCallback = lockFn;
    i2cUnlockCallback = unlockFn;
  } else {
    i2cLockCallback = nullptr;
    i2cUnlockCallback = nullptr;
  }
  portEXIT_CRITICAL(&hmiApiMux);
}

void hmiPlayCue(BuzzerCue cue) {
  portENTER_CRITICAL(&hmiApiMux);
  cueInbox = cue;
  cueInboxPending = true;
  markApiWorkPending();
  portEXIT_CRITICAL(&hmiApiMux);
}
