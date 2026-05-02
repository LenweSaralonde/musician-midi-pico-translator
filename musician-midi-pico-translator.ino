/**
 * Musician MIDI Pico Translator — USB Composite Device
 * - Standard USB MIDI (UART → USB byte-level thru)
 * - HID Keyboard with NKRO (N-Key Rollover)
 * - Mode persisted in EEPROM (flash emulation)
 * - LED mode animation and MIDI activity monitor
 *
 * Requirements:
 *   Board: Raspberry Pi Pico/RP2040 by Earle Philhower
 *   USB Stack: Adafruit TinyUSB
 *   Port: UF2_Board
 *
 * MIDI IN wiring (4N35 optocoupler):
 *   MIDI DIN-5 pin 4 ──[220Ω]──→ pin 1 (Anode)
 *   MIDI DIN-5 pin 5 ──────────→ pin 2 (Cathode)
 *                                pin 4 (Emitter)   → GND
 *                                pin 5 (Collector) → GP1 + 470Ω pull-up to 3.3V
 *
 * Declaration order (strict — Arduino preprocessor cannot forward-declare
 * functions that take enum/struct parameters):
 *
 *  1. Includes
 *  2. HID descriptor + USB objects
 *  3. enum Mode          ← used by animStart, modeSave, btnUpdate
 *  4. enum Key + tables  ← used by hidForKey, hidDispatch
 *  5. hidForKey()        ← uses Key, Mode
 *  6. USB MIDI ring      ← used by thruByte, ringDrain
 *  7. UART ISR + init
 *  8. NKRO key state
 *  9. LED (animStart uses Mode — must come after enum Mode)
 * 10. MIDI helpers
 * 11. EEPROM (uses Mode)
 * 12. Thru pipeline
 * 13. HID pipeline       ← uses hidForKey, holdKey, etc.
 * 14. BOOTSEL button     ← uses animStart, Mode, modeSave
 * 15. setup / loop
 */

// ─────────────────────────────────────────────────────────────────────────────
// 1. Includes
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <EEPROM.h>
#include <hardware/uart.h>
#include <hardware/irq.h>
#include <hardware/gpio.h>

// ─────────────────────────────────────────────────────────────────────────────
// 2. HID descriptor + USB objects
// ─────────────────────────────────────────────────────────────────────────────

#define USB_VENDOR_ID     0x1209 // pid.codes open source vendor
#define USB_VENDOR_NAME   "Generic"

#define USB_PRODUCT_ID    0xBA55 // 🎸
#define USB_PRODUCT_NAME  "Musician MIDI Pico Translator"

#define REPORT_ID_NKRO    1
#define NKRO_BITMAP_BYTES 16

static const uint8_t hid_desc[] = {
  HID_USAGE_PAGE(HID_USAGE_PAGE_DESKTOP),
  HID_USAGE(HID_USAGE_DESKTOP_KEYBOARD),
  HID_COLLECTION(HID_COLLECTION_APPLICATION),
    HID_REPORT_ID(REPORT_ID_NKRO)
    HID_USAGE_PAGE(HID_USAGE_PAGE_KEYBOARD),
    HID_USAGE_MIN(224), HID_USAGE_MAX(231),
    HID_LOGICAL_MIN(0), HID_LOGICAL_MAX(1),
    HID_REPORT_COUNT(8), HID_REPORT_SIZE(1),
    HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),
    HID_USAGE_PAGE(HID_USAGE_PAGE_KEYBOARD),
    HID_USAGE_MIN(0), HID_USAGE_MAX(127),
    HID_LOGICAL_MIN(0), HID_LOGICAL_MAX(1),
    HID_REPORT_COUNT(128), HID_REPORT_SIZE(1),
    HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),
  HID_COLLECTION_END
};

struct __attribute__((packed)) NKROReport {
  uint8_t modifier;
  uint8_t bitmap[NKRO_BITMAP_BYTES];
};
static NKROReport nkroReport = {};

Adafruit_USBD_HID  usb_hid(hid_desc, sizeof(hid_desc),
                            HID_ITF_PROTOCOL_NONE, 1, false);
Adafruit_USBD_MIDI usb_midi;

// ─────────────────────────────────────────────────────────────────────────────
// 3. enum Mode
// ─────────────────────────────────────────────────────────────────────────────

enum Mode : uint8_t { QWERTY=1, AZERTY=2, QWERTZ=3, DISABLED=4 };
static Mode currentMode = DISABLED;

// ─────────────────────────────────────────────────────────────────────────────
// 4. enum Key + note/layout tables
// ─────────────────────────────────────────────────────────────────────────────

#define NOTE_MIN   28
#define NOTE_MAX  103
#define NOTE_COUNT 76

enum Key : uint8_t {
  K_ENTER=0,K_DELETE,K_ESCAPE,K_BACKSPACE,K_TAB,
  K_NPDEC,K_NPADD,K_NPSUB,K_NPMUL,K_NPDIV,
  K_NP0,K_NP1,K_NP2,K_NP3,K_NP4,K_NP5,K_NP6,K_NP7,K_NP8,K_NP9,
  K_A,K_B,K_C,K_D,K_E,K_F,K_G,K_H,K_I,K_J,
  K_K,K_L,K_M,K_N,K_O,K_P,K_Q,K_R,K_S,K_T,
  K_U,K_V,K_W,K_X,K_Y,K_Z,
  K_1,K_2,K_3,K_4,K_5,K_6,K_7,K_8,K_9,K_0,
  K_F1,K_F2,K_F3,K_F4,K_F5,K_F6,K_F7,K_F8,K_F9,K_F10,K_F11,K_F12,
  K_HOME,K_END,K_PGDN,K_PGUP,
  K_LEFT,K_UP,K_RIGHT,K_DOWN,
  K_COUNT
};

static const Key noteToKey[NOTE_COUNT] = {
  K_ENTER,K_DELETE,K_ESCAPE,K_BACKSPACE,K_TAB,
  K_NPDEC,K_NPADD,K_NPSUB,K_NPMUL,K_NPDIV,
  K_NP0,K_NP1,K_NP2,K_NP3,K_NP4,K_NP5,K_NP6,K_NP7,K_NP8,K_NP9,
  K_A,K_B,K_C,K_D,K_E,K_F,K_G,K_H,K_I,K_J,
  K_K,K_L,K_M,K_N,K_O,K_P,K_Q,K_R,K_S,K_T,
  K_U,K_V,K_W,K_X,K_Y,K_Z,
  K_1,K_2,K_3,K_4,K_5,K_6,K_7,K_8,K_9,K_0,
  K_F1,K_F2,K_F3,K_F4,K_F5,K_F6,K_F7,K_F8,K_F9,K_F10,K_F11,K_F12,
  K_HOME,K_END,K_PGDN,K_PGUP,
  K_LEFT,K_UP,K_RIGHT,K_DOWN,
};

static const uint8_t layoutQWERTY[K_COUNT] = {
  HID_KEY_ENTER,HID_KEY_DELETE,HID_KEY_ESCAPE,HID_KEY_BACKSPACE,HID_KEY_TAB,
  HID_KEY_KEYPAD_DECIMAL,HID_KEY_KEYPAD_ADD,HID_KEY_KEYPAD_SUBTRACT,
  HID_KEY_KEYPAD_MULTIPLY,HID_KEY_KEYPAD_DIVIDE,
  HID_KEY_KEYPAD_0,HID_KEY_KEYPAD_1,HID_KEY_KEYPAD_2,HID_KEY_KEYPAD_3,HID_KEY_KEYPAD_4,
  HID_KEY_KEYPAD_5,HID_KEY_KEYPAD_6,HID_KEY_KEYPAD_7,HID_KEY_KEYPAD_8,HID_KEY_KEYPAD_9,
  HID_KEY_A,HID_KEY_B,HID_KEY_C,HID_KEY_D,HID_KEY_E,HID_KEY_F,HID_KEY_G,
  HID_KEY_H,HID_KEY_I,HID_KEY_J,HID_KEY_K,HID_KEY_L,HID_KEY_M,HID_KEY_N,
  HID_KEY_O,HID_KEY_P,HID_KEY_Q,HID_KEY_R,HID_KEY_S,HID_KEY_T,HID_KEY_U,
  HID_KEY_V,HID_KEY_W,HID_KEY_X,HID_KEY_Y,HID_KEY_Z,
  HID_KEY_1,HID_KEY_2,HID_KEY_3,HID_KEY_4,HID_KEY_5,
  HID_KEY_6,HID_KEY_7,HID_KEY_8,HID_KEY_9,HID_KEY_0,
  HID_KEY_F1,HID_KEY_F2,HID_KEY_F3,HID_KEY_F4,HID_KEY_F5,HID_KEY_F6,
  HID_KEY_F7,HID_KEY_F8,HID_KEY_F9,HID_KEY_F10,HID_KEY_F11,HID_KEY_F12,
  HID_KEY_HOME,HID_KEY_END,HID_KEY_PAGE_DOWN,HID_KEY_PAGE_UP,
  HID_KEY_ARROW_LEFT,HID_KEY_ARROW_UP,HID_KEY_ARROW_RIGHT,HID_KEY_ARROW_DOWN,
};

static const uint8_t layoutAZERTY[K_COUNT] = {
  HID_KEY_ENTER,HID_KEY_DELETE,HID_KEY_ESCAPE,HID_KEY_BACKSPACE,HID_KEY_TAB,
  HID_KEY_KEYPAD_DECIMAL,HID_KEY_KEYPAD_ADD,HID_KEY_KEYPAD_SUBTRACT,
  HID_KEY_KEYPAD_MULTIPLY,HID_KEY_KEYPAD_DIVIDE,
  HID_KEY_KEYPAD_0,HID_KEY_KEYPAD_1,HID_KEY_KEYPAD_2,HID_KEY_KEYPAD_3,HID_KEY_KEYPAD_4,
  HID_KEY_KEYPAD_5,HID_KEY_KEYPAD_6,HID_KEY_KEYPAD_7,HID_KEY_KEYPAD_8,HID_KEY_KEYPAD_9,
  HID_KEY_Q,         // A → physical Q on AZERTY
  HID_KEY_B,HID_KEY_C,HID_KEY_D,HID_KEY_E,HID_KEY_F,HID_KEY_G,
  HID_KEY_H,HID_KEY_I,HID_KEY_J,HID_KEY_K,HID_KEY_L,
  HID_KEY_SEMICOLON, // M → physical ; on AZERTY
  HID_KEY_N,HID_KEY_O,HID_KEY_P,
  HID_KEY_A,         // Q → physical A on AZERTY
  HID_KEY_R,HID_KEY_S,HID_KEY_T,HID_KEY_U,HID_KEY_V,
  HID_KEY_Z,         // W → physical Z on AZERTY
  HID_KEY_X,HID_KEY_Y,
  HID_KEY_W,         // Z → physical W on AZERTY
  HID_KEY_1,HID_KEY_2,HID_KEY_3,HID_KEY_4,HID_KEY_5,
  HID_KEY_6,HID_KEY_7,HID_KEY_8,HID_KEY_9,HID_KEY_0,
  HID_KEY_F1,HID_KEY_F2,HID_KEY_F3,HID_KEY_F4,HID_KEY_F5,HID_KEY_F6,
  HID_KEY_F7,HID_KEY_F8,HID_KEY_F9,HID_KEY_F10,HID_KEY_F11,HID_KEY_F12,
  HID_KEY_HOME,HID_KEY_END,HID_KEY_PAGE_DOWN,HID_KEY_PAGE_UP,
  HID_KEY_ARROW_LEFT,HID_KEY_ARROW_UP,HID_KEY_ARROW_RIGHT,HID_KEY_ARROW_DOWN,
};

static const uint8_t layoutQWERTZ[K_COUNT] = {
  HID_KEY_ENTER,HID_KEY_DELETE,HID_KEY_ESCAPE,HID_KEY_BACKSPACE,HID_KEY_TAB,
  HID_KEY_KEYPAD_DECIMAL,HID_KEY_KEYPAD_ADD,HID_KEY_KEYPAD_SUBTRACT,
  HID_KEY_KEYPAD_MULTIPLY,HID_KEY_KEYPAD_DIVIDE,
  HID_KEY_KEYPAD_0,HID_KEY_KEYPAD_1,HID_KEY_KEYPAD_2,HID_KEY_KEYPAD_3,HID_KEY_KEYPAD_4,
  HID_KEY_KEYPAD_5,HID_KEY_KEYPAD_6,HID_KEY_KEYPAD_7,HID_KEY_KEYPAD_8,HID_KEY_KEYPAD_9,
  HID_KEY_A,HID_KEY_B,HID_KEY_C,HID_KEY_D,HID_KEY_E,HID_KEY_F,HID_KEY_G,
  HID_KEY_H,HID_KEY_I,HID_KEY_J,HID_KEY_K,HID_KEY_L,HID_KEY_M,HID_KEY_N,
  HID_KEY_O,HID_KEY_P,HID_KEY_Q,HID_KEY_R,HID_KEY_S,HID_KEY_T,HID_KEY_U,
  HID_KEY_V,HID_KEY_W,HID_KEY_X,
  HID_KEY_Z,HID_KEY_Y, // Y↔Z swapped on QWERTZ
  HID_KEY_1,HID_KEY_2,HID_KEY_3,HID_KEY_4,HID_KEY_5,
  HID_KEY_6,HID_KEY_7,HID_KEY_8,HID_KEY_9,HID_KEY_0,
  HID_KEY_F1,HID_KEY_F2,HID_KEY_F3,HID_KEY_F4,HID_KEY_F5,HID_KEY_F6,
  HID_KEY_F7,HID_KEY_F8,HID_KEY_F9,HID_KEY_F10,HID_KEY_F11,HID_KEY_F12,
  HID_KEY_HOME,HID_KEY_END,HID_KEY_PAGE_DOWN,HID_KEY_PAGE_UP,
  HID_KEY_ARROW_LEFT,HID_KEY_ARROW_UP,HID_KEY_ARROW_RIGHT,HID_KEY_ARROW_DOWN,
};

// ─────────────────────────────────────────────────────────────────────────────
// 5. hidForKey()  — needs Key and Mode, both now declared above
// ─────────────────────────────────────────────────────────────────────────────

static uint8_t hidForKey(Key k) {
  switch (currentMode) {
    case QWERTY: return layoutQWERTY[k];
    case AZERTY: return layoutAZERTY[k];
    case QWERTZ: return layoutQWERTZ[k];
    default:     return 0;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. USB MIDI ring buffer  — used by thruByte
// ─────────────────────────────────────────────────────────────────────────────

#define RING_SIZE 128
static uint8_t ring[RING_SIZE][4];
static uint8_t ringHead  = 0;
static uint8_t ringTail  = 0;
static uint8_t ringCount = 0;

static void ringPush(uint8_t cin, uint8_t b0, uint8_t b1, uint8_t b2) {
  if (ringCount >= RING_SIZE) return;
  ring[ringHead][0] = cin; ring[ringHead][1] = b0;
  ring[ringHead][2] = b1;  ring[ringHead][3] = b2;
  ringHead  = (ringHead + 1) & (RING_SIZE - 1);
  ringCount++;
}

static void ringDrain() {
  if (!TinyUSBDevice.mounted()) {
    ringHead = ringTail = ringCount = 0; return;
  }
  while (ringCount > 0) {
    if (tud_midi_n_stream_write(0, 0, ring[ringTail], 4) == 0) break;
    ringTail  = (ringTail + 1) & (RING_SIZE - 1);
    ringCount--;
  }
  if (ringCount == 0) usb_midi.flush();
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. Interrupt-driven UART RX
// ─────────────────────────────────────────────────────────────────────────────

#define MIDI_UART_HW  uart0
#define MIDI_BAUD     31250
#define SW_FIFO_SIZE  256

static uint8_t           swFifo[SW_FIFO_SIZE];
static volatile uint16_t swHead = 0;
static volatile uint16_t swTail = 0;

static inline bool swEmpty() { return swHead == swTail; }

static void uartRxISR() {
  while (uart_is_readable(MIDI_UART_HW)) {
    uint8_t b     = (uint8_t)uart_getc(MIDI_UART_HW);
    uint16_t next = (swHead + 1) & (SW_FIFO_SIZE - 1);
    if (next != swTail) { swFifo[swHead] = b; swHead = next; }
  }
}

static void uartInit() {
  uart_init(MIDI_UART_HW, MIDI_BAUD);
  gpio_set_function(1, GPIO_FUNC_UART);
  gpio_set_function(0, GPIO_FUNC_UART);
  uart_set_hw_flow(MIDI_UART_HW, false, false);
  uart_set_format(MIDI_UART_HW, 8, 1, UART_PARITY_NONE);
  uart_set_fifo_enabled(MIDI_UART_HW, true);
  irq_set_exclusive_handler(UART0_IRQ, uartRxISR);
  irq_set_enabled(UART0_IRQ, true);
  uart_set_irq_enables(MIDI_UART_HW, true, false);
}

// ─────────────────────────────────────────────────────────────────────────────
// 8. NKRO HID key state
// ─────────────────────────────────────────────────────────────────────────────

static void sendHIDReport() {
  if (!usb_hid.ready()) return;
  usb_hid.sendReport(REPORT_ID_NKRO, &nkroReport, sizeof(nkroReport));
}

static void bitmapSet(uint8_t kc) {
  if (kc >= 224 && kc <= 231)  nkroReport.modifier       |=  (1 << (kc - 224));
  else if (kc > 0 && kc < 128) nkroReport.bitmap[kc >> 3] |=  (1 << (kc & 7));
}
static void bitmapClear(uint8_t kc) {
  if (kc >= 224 && kc <= 231)  nkroReport.modifier       &= ~(1 << (kc - 224));
  else if (kc > 0 && kc < 128) nkroReport.bitmap[kc >> 3] &= ~(1 << (kc & 7));
}

static void holdKey(uint8_t kc)    { if (!kc) return; bitmapSet(kc);   sendHIDReport(); }
static void releaseKey(uint8_t kc) { if (!kc) return; bitmapClear(kc); sendHIDReport(); }
static void releaseAllKeys()       { memset(&nkroReport,0,sizeof(nkroReport)); sendHIDReport(); }
static void holdSpace()            { bitmapSet(HID_KEY_SPACE);   sendHIDReport(); }
static void releaseSpace()         { bitmapClear(HID_KEY_SPACE); sendHIDReport(); }

// ─────────────────────────────────────────────────────────────────────────────
// 9. LED  — animStart(Mode) now safe: Mode declared at step 3
// ─────────────────────────────────────────────────────────────────────────────

#define LONG_MS  500u
#define SHORT_MS 120u
#define GAP_MS   700u

struct Pulse { uint16_t onMs; uint16_t offMs; };
static const Pulse anim1[] = { {LONG_MS,SHORT_MS},{SHORT_MS,GAP_MS} };
static const Pulse anim2[] = { {LONG_MS,SHORT_MS},{SHORT_MS,SHORT_MS},{SHORT_MS,GAP_MS} };
static const Pulse anim3[] = { {LONG_MS,SHORT_MS},{SHORT_MS,SHORT_MS},{SHORT_MS,SHORT_MS},{SHORT_MS,GAP_MS} };
static const Pulse anim4[] = { {LONG_MS,GAP_MS} };

struct AnimDef { const Pulse *p; uint8_t n; };
static const AnimDef anims[5] = {
  {nullptr,0},
  {anim1, sizeof(anim1)/sizeof(anim1[0])},
  {anim2, sizeof(anim2)/sizeof(anim2[0])},
  {anim3, sizeof(anim3)/sizeof(anim3[0])},
  {anim4, sizeof(anim4)/sizeof(anim4[0])},
};

static bool     animRunning = false;
static uint8_t  animIdx     = 0;
static bool     animInOn    = false;
static uint32_t animUntil   = 0;

static void animStart(Mode m) {
  animRunning = true;
  animIdx     = 0;
  animInOn    = true;
  animUntil   = millis() + anims[m].p[0].onMs;
  digitalWrite(LED_BUILTIN, HIGH);
}

static void animUpdate() {
  if (!animRunning) return;
  if (millis() < animUntil) return;
  const AnimDef &def = anims[currentMode];
  if (animInOn) {
    animInOn  = false;
    animUntil = millis() + def.p[animIdx].offMs;
    digitalWrite(LED_BUILTIN, LOW);
  } else {
    if (++animIdx >= def.n) {
      animRunning = false;
      digitalWrite(LED_BUILTIN, LOW);
      return;
    }
    animInOn  = true;
    animUntil = millis() + def.p[animIdx].onMs;
    digitalWrite(LED_BUILTIN, HIGH);
  }
}

// MIDI activity monitor — only active when no animation is playing
#define MIDI_LED_MS   20u
static uint32_t midiLedOffAt = 0;

static void ledMidiActivity(uint8_t b) {
  if (animRunning) return;
  if (b >= 0xF8) return;  // ignore clock and all realtime messages
  digitalWrite(LED_BUILTIN, HIGH);
  midiLedOffAt = millis() + MIDI_LED_MS;
}

static void ledUpdate() {
  if (animRunning) return;
  if (midiLedOffAt && millis() >= midiLedOffAt) {
    digitalWrite(LED_BUILTIN, LOW);
    midiLedOffAt = 0;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// 10. MIDI helpers
// ─────────────────────────────────────────────────────────────────────────────

static uint8_t midiDataBytes(uint8_t status) {
  switch (status & 0xF0) {
    case 0x80: case 0x90: case 0xA0:
    case 0xB0: case 0xE0: return 2;
    case 0xC0: case 0xD0: return 1;
    case 0xF0:
      if (status == 0xF2) return 2;
      if (status == 0xF3) return 1;
      return 0;
    default: return 0;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// 11. EEPROM persistence  — uses Mode (declared at step 3)
// ─────────────────────────────────────────────────────────────────────────────

#define EEPROM_SIZE        4
#define EEPROM_MAGIC       0xA5
#define EEPROM_ADDR_MAGIC  0
#define EEPROM_ADDR_MODE   1

static void modeSave(uint8_t mode) {
  EEPROM.write(EEPROM_ADDR_MAGIC, EEPROM_MAGIC);
  EEPROM.write(EEPROM_ADDR_MODE,  mode);
  EEPROM.commit();
}

static uint8_t modeLoad() {
  EEPROM.begin(EEPROM_SIZE);
  if (EEPROM.read(EEPROM_ADDR_MAGIC) != EEPROM_MAGIC) return 4; // DISABLED
  uint8_t m = EEPROM.read(EEPROM_ADDR_MODE);
  if (m < 1 || m > 4) return 4;
  return m;
}

// ─────────────────────────────────────────────────────────────────────────────
// 12. Pipeline 1 — MIDI thru
// ─────────────────────────────────────────────────────────────────────────────

#define SYSEX_MAX 512u

static uint8_t  thru_status  = 0;
static uint8_t  thru_dExp    = 0;
static uint8_t  thru_d[2]    = {};
static uint8_t  thru_dLen    = 0;
static bool     thru_inSx    = false;
static uint8_t  thru_sx[3]   = {};
static uint8_t  thru_sxLen   = 0;
static uint16_t thru_sxTotal = 0;

static uint8_t thruCIN(uint8_t s) {
  switch (s & 0xF0) {
    case 0x80: return 0x08; case 0x90: return 0x09;
    case 0xA0: return 0x0A; case 0xB0: return 0x0B;
    case 0xC0: return 0x0C; case 0xD0: return 0x0D;
    case 0xE0: return 0x0E;
    case 0xF0:
      if (s == 0xF2) return 0x03;
      if (s == 0xF3) return 0x02;
      return 0x05;
    default: return 0x0F;
  }
}

static void thruSxFlush(bool end) {
  if (thru_sxLen == 0) { thru_inSx = false; return; }
  if (end) {
    ringPush((uint8_t)(0x04 + thru_sxLen),
             thru_sx[0],
             thru_sxLen > 1 ? thru_sx[1] : 0,
             thru_sxLen > 2 ? thru_sx[2] : 0);
    thru_sxLen = 0; thru_inSx = false; thru_sxTotal = 0;
  } else {
    ringPush(0x04, thru_sx[0], thru_sx[1], thru_sx[2]);
    thru_sxLen = 0;
  }
}

static void thruByte(uint8_t b) {
  if (b >= 0xF8) { ringPush(0x0F, b, 0, 0); return; }

  if (thru_inSx) {
    if (++thru_sxTotal > SYSEX_MAX) {
      thru_inSx = false; thru_sxLen = 0; thru_sxTotal = 0; thru_status = 0; return;
    }
    thru_sx[thru_sxLen++] = b;
    if (b == 0xF7 || thru_sxLen == 3) thruSxFlush(b == 0xF7);
    return;
  }

  if (b & 0x80) {
    if (b == 0xF0) {
      thru_inSx = true; thru_sxLen = 0; thru_sxTotal = 1;
      thru_sx[thru_sxLen++] = 0xF0; return;
    }
    if (b == 0xF7) return;
    thru_status = b; thru_dExp = midiDataBytes(b); thru_dLen = 0;
    if (thru_dExp == 0) {
      ringPush(thruCIN(b), b, 0, 0);
      if (b >= 0xF1) thru_status = 0;
    }
    return;
  }

  if (!thru_status) return;
  if (thru_dLen < 2) thru_d[thru_dLen] = b;
  thru_dLen++;
  if (thru_dLen == thru_dExp) {
    ringPush(thruCIN(thru_status), thru_status,
             thru_d[0], thru_dExp > 1 ? thru_d[1] : 0);
    thru_dLen = 0;
    if (thru_status >= 0xF1) thru_status = 0;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// 13. Pipeline 2 — HID parser  — hidForKey/holdKey/etc. all declared above
// ─────────────────────────────────────────────────────────────────────────────

static uint8_t  hid_status  = 0;
static uint8_t  hid_dExp    = 0;
static uint8_t  hid_d[2]    = {};
static uint8_t  hid_dLen    = 0;
static bool     hid_inSx    = false;
static uint16_t hid_sxTotal = 0;

static void hidDispatch() {
  if (currentMode == DISABLED) return;
  uint8_t type = hid_status & 0xF0;
  uint8_t d1   = hid_d[0];
  uint8_t d2   = hid_dLen > 1 ? hid_d[1] : 0;
  if (type == 0x90 && d2 > 0) {
    if (d1 >= NOTE_MIN && d1 <= NOTE_MAX)
      holdKey(hidForKey(noteToKey[d1 - NOTE_MIN]));
  } else if (type == 0x80 || (type == 0x90 && d2 == 0)) {
    if (d1 >= NOTE_MIN && d1 <= NOTE_MAX)
      releaseKey(hidForKey(noteToKey[d1 - NOTE_MIN]));
  } else if (type == 0xB0 && d1 == 64) {
    if (d2 >= 64) holdSpace(); else releaseSpace();
  }
}

static void hidByte(uint8_t b) {
  if (b >= 0xF8) return;
  if (hid_inSx) {
    if (++hid_sxTotal > SYSEX_MAX) {
      hid_inSx = false; hid_sxTotal = 0; hid_status = 0;
    } else if (b == 0xF7) hid_inSx = false;
    return;
  }
  if (b & 0x80) {
    if (b == 0xF0) { hid_inSx = true; hid_sxTotal = 1; return; }
    if (b == 0xF7) return;
    hid_status = b; hid_dExp = midiDataBytes(b); hid_dLen = 0;
    if (b >= 0xF1) hid_status = 0;
    return;
  }
  if (!hid_status) return;
  if (hid_dLen < 2) hid_d[hid_dLen] = b;
  hid_dLen++;
  if (hid_dLen == hid_dExp) {
    hidDispatch();
    hid_dLen = 0;
    if (hid_status >= 0xF1) hid_status = 0;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// 14. BOOTSEL button  — animStart/Mode/modeSave all declared above
// ─────────────────────────────────────────────────────────────────────────────

#define BOOTSEL_POLL_MS  5u
#define DEBOUNCE_MS     40u

static bool     btn_lastRaw  = false;
static bool     btn_state    = false;
static uint32_t btn_lastEdge = 0;

static void btnUpdate() {
  static uint32_t lastPoll = 0;
  uint32_t now = millis();
  if (now - lastPoll < BOOTSEL_POLL_MS) return;
  lastPoll = now;
  bool raw = rp2040.isPicoW() ? false : (bool)BOOTSEL;
  if (raw != btn_lastRaw) { btn_lastRaw = raw; btn_lastEdge = now; }
  if (now - btn_lastEdge < DEBOUNCE_MS) return;
  if (raw && !btn_state) {
    btn_state   = true;
    currentMode = (Mode)((currentMode % 4) + 1);
    releaseAllKeys();
    modeSave((uint8_t)currentMode);
    animStart(currentMode);
  } else if (!raw && btn_state) {
    btn_state = false;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// 15. Setup & loop
// ─────────────────────────────────────────────────────────────────────────────

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  currentMode = (Mode)modeLoad();

  TinyUSBDevice.setProductDescriptor(USB_PRODUCT_NAME);
  TinyUSBDevice.setManufacturerDescriptor(USB_VENDOR_NAME);
  TinyUSBDevice.setID(USB_VENDOR_ID, USB_PRODUCT_ID); 

  usb_hid.begin();
  usb_midi.begin();

  while (!TinyUSBDevice.mounted()) delay(1);
  delay(300);

  uartInit();

  animStart(currentMode);
  while (animRunning) { animUpdate(); yield(); }
}

void loop() {
  while (!swEmpty()) {
    irq_set_enabled(UART0_IRQ, false);
    uint8_t b = swFifo[swTail];
    swTail = (swTail + 1) & (SW_FIFO_SIZE - 1);
    irq_set_enabled(UART0_IRQ, true);
    thruByte(b);
    hidByte(b);
    ledMidiActivity(b);
  }
  ringDrain();
  btnUpdate();
  animUpdate();
  ledUpdate();
  yield();
}
