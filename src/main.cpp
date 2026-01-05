#include <Arduino.h>
#include <EInkDisplay.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <InputManager.h>
#include <SDCardManager.h>
#include <SPI.h>
#include <builtinFonts/all.h>

#include <memory>
#include <string>
#include <utility>

#include "Battery.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "activities/boot_sleep/BootActivity.h"
#include "activities/boot_sleep/SleepActivity.h"
#include "activities/home/HomeActivity.h"
#include "activities/network/CrossPointWebServerActivity.h"
#include "activities/reader/ReaderActivity.h"
#include "activities/settings/SettingsActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "fontIds.h"

namespace cp {
// C++11-compatible make_unique replacement
template <typename T, typename... Args>
static inline std::unique_ptr<T> make_unique(Args&&... args) {
  return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}
}  // namespace cp

namespace BoardPins {
constexpr int EPD_SCLK = 8;   // SPI Clock
constexpr int EPD_MOSI = 10;  // SPI MOSI
constexpr int EPD_CS = 21;    // Chip Select
constexpr int EPD_DC = 4;     // Data/Command
constexpr int EPD_RST = 5;    // Reset
constexpr int EPD_BUSY = 6;   // Busy

constexpr int SD_SPI_MISO = 7;

// Used for USB connection detection
constexpr int UART0_RXD = 20;
}  // namespace BoardPins

static EInkDisplay einkDisplay(BoardPins::EPD_SCLK, BoardPins::EPD_MOSI, BoardPins::EPD_CS, BoardPins::EPD_DC,
                               BoardPins::EPD_RST, BoardPins::EPD_BUSY);
static InputManager inputManager;
static MappedInputManager mappedInputManager(inputManager);
static GfxRenderer renderer(einkDisplay);

static std::unique_ptr<Activity> currentActivity;
static std::string g_readerStartPath;

// ---------- Font packs (removes boilerplate) ----------
using BuiltinFontT = decltype(bookerly_12_regular);

template <typename F>
struct FontPack4 {
  EpdFont regular;
  EpdFont bold;
  EpdFont italic;
  EpdFont boldItalic;
  EpdFontFamily family;

  FontPack4(const F* r, const F* b, const F* i, const F* bi)
      : regular(r), bold(b), italic(i), boldItalic(bi), family(&regular, &bold, &italic, &boldItalic) {}
};

template <typename F>
struct FontPack2 {
  EpdFont regular;
  EpdFont bold;
  EpdFontFamily family;

  FontPack2(const F* r, const F* b) : regular(r), bold(b), family(&regular, &bold) {}
};

template <typename F>
struct FontPack1 {
  EpdFont regular;
  EpdFontFamily family;

  explicit FontPack1(const F* r) : regular(r), family(&regular) {}
};

static FontPack4<BuiltinFontT> bookerly12(&bookerly_12_regular, &bookerly_12_bold, &bookerly_12_italic,
                                          &bookerly_12_bolditalic);
static FontPack4<BuiltinFontT> bookerly14(&bookerly_14_regular, &bookerly_14_bold, &bookerly_14_italic,
                                          &bookerly_14_bolditalic);
static FontPack4<BuiltinFontT> bookerly16(&bookerly_16_regular, &bookerly_16_bold, &bookerly_16_italic,
                                          &bookerly_16_bolditalic);
static FontPack4<BuiltinFontT> bookerly18(&bookerly_18_regular, &bookerly_18_bold, &bookerly_18_italic,
                                          &bookerly_18_bolditalic);

static FontPack4<BuiltinFontT> notosans12(&notosans_12_regular, &notosans_12_bold, &notosans_12_italic,
                                          &notosans_12_bolditalic);
static FontPack4<BuiltinFontT> notosans14(&notosans_14_regular, &notosans_14_bold, &notosans_14_italic,
                                          &notosans_14_bolditalic);
static FontPack4<BuiltinFontT> notosans16(&notosans_16_regular, &notosans_16_bold, &notosans_16_italic,
                                          &notosans_16_bolditalic);
static FontPack4<BuiltinFontT> notosans18(&notosans_18_regular, &notosans_18_bold, &notosans_18_italic,
                                          &notosans_18_bolditalic);

static FontPack4<BuiltinFontT> opendyslexic8(&opendyslexic_8_regular, &opendyslexic_8_bold, &opendyslexic_8_italic,
                                             &opendyslexic_8_bolditalic);
static FontPack4<BuiltinFontT> opendyslexic10(&opendyslexic_10_regular, &opendyslexic_10_bold, &opendyslexic_10_italic,
                                              &opendyslexic_10_bolditalic);
static FontPack4<BuiltinFontT> opendyslexic12(&opendyslexic_12_regular, &opendyslexic_12_bold, &opendyslexic_12_italic,
                                              &opendyslexic_12_bolditalic);
static FontPack4<BuiltinFontT> opendyslexic14(&opendyslexic_14_regular, &opendyslexic_14_bold, &opendyslexic_14_italic,
                                              &opendyslexic_14_bolditalic);

static FontPack2<BuiltinFontT> ui10(&ubuntu_10_regular, &ubuntu_10_bold);
static FontPack2<BuiltinFontT> ui12(&ubuntu_12_regular, &ubuntu_12_bold);
static FontPack1<BuiltinFontT> small(&notosans_8_regular);

// ---------- Timing ----------
static uint32_t bootT0 = 0;
static uint32_t verifyT1 = 0;

template <typename... Args>
static inline void logf(const char* fmt, Args... args) {
  if (Serial) {
    Serial.printf(fmt, args...);
  }
}

static void switchActivity(std::unique_ptr<Activity> next) {
  if (currentActivity) currentActivity->onExit();
  currentActivity = std::move(next);
  if (currentActivity) currentActivity->onEnter();
}

static void waitForPowerRelease() {
  inputManager.update();
  while (inputManager.isPressed(InputManager::BTN_POWER)) {
    delay(50);
    inputManager.update();
  }
}

static inline void armWakeAndDeepSleepNow() {
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start();
}

static void verifyWakeupLongPress() {
  // Only enforce on deep-sleep wake
  if (esp_reset_reason() != ESP_RST_DEEPSLEEP) {
    return;
  }

  const uint32_t startMs = millis();
  const uint32_t requiredMs = static_cast<uint32_t>(SETTINGS.getPowerButtonDuration());

  inputManager.update();

  // Give up to 1000ms to begin holding power
  while (!inputManager.isPressed(InputManager::BTN_POWER) && (millis() - startMs < 1000)) {
    delay(10);
    inputManager.update();
  }

  verifyT1 = millis();

  if (!inputManager.isPressed(InputManager::BTN_POWER)) {
    armWakeAndDeepSleepNow();
  }

  // Require continuous hold for requiredMs
  while (inputManager.isPressed(InputManager::BTN_POWER) && inputManager.getHeldTime() < requiredMs) {
    delay(10);
    inputManager.update();
  }

  if (inputManager.getHeldTime() < requiredMs) {
    armWakeAndDeepSleepNow();
  }
}

// Forward declarations for callbacks
static void onGoHome();

static void openReader(const std::string& epubPath) {
  g_readerStartPath = epubPath;  // stable storage
  switchActivity(cp::make_unique<ReaderActivity>(renderer, mappedInputManager, g_readerStartPath, onGoHome));
}

static void onGoToReaderHome() { openReader(std::string()); }
static void onContinueReading() { openReader(APP_STATE.openEpubPath); }

static void onGoToFileTransfer() {
  switchActivity(cp::make_unique<CrossPointWebServerActivity>(renderer, mappedInputManager, onGoHome));
}

static void onGoToSettings() {
  switchActivity(cp::make_unique<SettingsActivity>(renderer, mappedInputManager, onGoHome));
}

static void onGoHome() {
  switchActivity(cp::make_unique<HomeActivity>(renderer, mappedInputManager, onContinueReading, onGoToReaderHome,
                                               onGoToSettings, onGoToFileTransfer));
}

static void setupDisplayAndFonts() {
  einkDisplay.begin();
  logf("[%lu] [   ] Display initialized\n", millis());

  struct FontReg {
    int32_t id;
    EpdFontFamily* family;
  };

  static FontReg regs[] = {
      {BOOKERLY_12_FONT_ID, &bookerly12.family},
      {BOOKERLY_14_FONT_ID, &bookerly14.family},
      {BOOKERLY_16_FONT_ID, &bookerly16.family},
      {BOOKERLY_18_FONT_ID, &bookerly18.family},

      {NOTOSANS_12_FONT_ID, &notosans12.family},
      {NOTOSANS_14_FONT_ID, &notosans14.family},
      {NOTOSANS_16_FONT_ID, &notosans16.family},
      {NOTOSANS_18_FONT_ID, &notosans18.family},

      {OPENDYSLEXIC_8_FONT_ID, &opendyslexic8.family},
      {OPENDYSLEXIC_10_FONT_ID, &opendyslexic10.family},
      {OPENDYSLEXIC_12_FONT_ID, &opendyslexic12.family},
      {OPENDYSLEXIC_14_FONT_ID, &opendyslexic14.family},

      {UI_10_FONT_ID, &ui10.family},
      {UI_12_FONT_ID, &ui12.family},
      {SMALL_FONT_ID, &small.family},
  };

  for (size_t i = 0; i < (sizeof(regs) / sizeof(regs[0])); ++i) {
    renderer.insertFont(regs[i].id, *regs[i].family);
  }

  logf("[%lu] [   ] Fonts setup\n", millis());
}

// Enter deep sleep mode (UI sleep activity + release protection)
static void enterDeepSleep() {
  switchActivity(cp::make_unique<SleepActivity>(renderer, mappedInputManager));

  einkDisplay.deepSleep();

  const uint32_t calib = (verifyT1 >= bootT0) ? (verifyT1 - bootT0) : 0;
  logf("[%lu] [   ] Power button press calibration value: %lu ms\n", millis(), static_cast<unsigned long>(calib));
  logf("[%lu] [   ] Entering deep sleep.\n", millis());

  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);

  // Avoid immediate wake if user is still holding power
  waitForPowerRelease();

  esp_deep_sleep_start();
}

void setup() {
  bootT0 = millis();
  verifyT1 = bootT0;

  // Only start serial if USB connected
  pinMode(BoardPins::UART0_RXD, INPUT);
  if (digitalRead(BoardPins::UART0_RXD) == HIGH) {
    Serial.begin(115200);
  }

  inputManager.begin();
  inputManager.update();

  pinMode(BAT_GPIO0, INPUT);

  SPI.begin(BoardPins::EPD_SCLK, BoardPins::SD_SPI_MISO, BoardPins::EPD_MOSI, BoardPins::EPD_CS);

  // SD Card init
  if (!SdMan.begin()) {
    logf("[%lu] [   ] SD card initialization failed\n", millis());
    setupDisplayAndFonts();
    switchActivity(
        cp::make_unique<FullScreenMessageActivity>(renderer, mappedInputManager, "SD card error", EpdFontFamily::BOLD));
    return;
  }

  SETTINGS.loadFromFile();
  verifyWakeupLongPress();

  logf("[%lu] [   ] Starting CrossPoint version " CROSSPOINT_VERSION "\n", millis());

  setupDisplayAndFonts();
  switchActivity(cp::make_unique<BootActivity>(renderer, mappedInputManager));

  APP_STATE.loadFromFile();
  if (APP_STATE.openEpubPath.empty()) {
    onGoHome();
  } else {
    const std::string path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath.clear();
    APP_STATE.saveToFile();
    openReader(path);
  }

  waitForPowerRelease();
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  static unsigned long lastMemPrint = 0;
  static unsigned long lastActivityTime = 0;

  const unsigned long loopStartTime = millis();
  inputManager.update();

  const unsigned long now = millis();

  if (Serial && (now - lastMemPrint >= 10000)) {
    logf("[%lu] [MEM] Free: %d bytes, Total: %d bytes, Min Free: %d bytes\n", now, ESP.getFreeHeap(), ESP.getHeapSize(),
         ESP.getMinFreeHeap());
    lastMemPrint = now;
  }

  if (lastActivityTime == 0) lastActivityTime = now;

  if (inputManager.wasAnyPressed() || inputManager.wasAnyReleased() ||
      (currentActivity && currentActivity->preventAutoSleep())) {
    lastActivityTime = now;
  }

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (now - lastActivityTime >= sleepTimeoutMs) {
    logf("[%lu] [SLP] Auto-sleep triggered after %lu ms of inactivity\n", now, sleepTimeoutMs);
    enterDeepSleep();
    return;
  }

  if (inputManager.isPressed(InputManager::BTN_POWER) &&
      inputManager.getHeldTime() > SETTINGS.getPowerButtonDuration()) {
    enterDeepSleep();
    return;
  }

  const unsigned long activityStartTime = millis();
  if (currentActivity) currentActivity->loop();
  const unsigned long activityDuration = millis() - activityStartTime;

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      logf("[%lu] [LOOP] New max loop duration: %lu ms (activity: %lu ms)\n", millis(), maxLoopDuration,
           activityDuration);
    }
  }

  if (currentActivity && currentActivity->skipLoopDelay()) {
    yield();
  } else {
    delay(10);
  }
}
