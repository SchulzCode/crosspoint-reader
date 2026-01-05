#include "CrossPointSettings.h"

#include <Arduino.h>
#include <HardwareSerial.h>
#include <SDCardManager.h>
#include <Serialization.h>

#include <stdarg.h>
#include <stdio.h>

#include "fontIds.h"

// Initialize the static instance
CrossPointSettings CrossPointSettings::instance;

namespace {
constexpr uint8_t SETTINGS_FILE_VERSION = 1;
constexpr char SETTINGS_FILE[] = "/.crosspoint/settings.bin";

// Single source of truth: persisted fields + order.
#define CPS_FIELDS(X)                \
  X(sleepScreen)                     \
  X(extraParagraphSpacing)           \
  X(shortPwrBtn)                     \
  X(statusBar)                       \
  X(orientation)                     \
  X(frontButtonLayout)               \
  X(sideButtonLayout)                \
  X(fontFamily)                      \
  X(fontSize)                        \
  X(lineSpacing)                     \
  X(paragraphAlignment)              \
  X(sleepTimeout)                    \
  X(refreshFrequency)                \
  X(screenMargin)                    \
  X(sleepScreenCoverMode)

#define CPS_COUNT_FIELD(name) + 1
constexpr uint8_t SETTINGS_COUNT = static_cast<uint8_t>(0 CPS_FIELDS(CPS_COUNT_FIELD));
#undef CPS_COUNT_FIELD

static inline void logf(const char* fmt, ...) {
  if (!Serial) return;

  char buf[192];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  Serial.print(buf);
}

template <typename T>
static inline bool readIfAvailable(FsFile& f, uint8_t toRead, uint8_t& readCount, T& out) {
  if (readCount >= toRead) return false;
  serialization::readPod(f, out);
  ++readCount;
  return true;
}

static inline float compressionFor(uint8_t spacing, float tight, float normal, float wide) {
  switch (spacing) {
    case CrossPointSettings::TIGHT:
      return tight;
    case CrossPointSettings::WIDE:
      return wide;
    case CrossPointSettings::NORMAL:
    default:
      return normal;
  }
}

static inline unsigned long minutesToMs(unsigned long minutes) {
  return minutes * 60UL * 1000UL;
}

static inline int fontIdForSize(uint8_t size, int smallId, int mediumId, int largeId, int extraLargeId) {
  switch (size) {
    case CrossPointSettings::SMALL:
      return smallId;
    case CrossPointSettings::LARGE:
      return largeId;
    case CrossPointSettings::EXTRA_LARGE:
      return extraLargeId;
    case CrossPointSettings::MEDIUM:
    default:
      return mediumId;
  }
}
}  // namespace

bool CrossPointSettings::saveToFile() const {
  SdMan.mkdir("/.crosspoint");

  FsFile outputFile;
  if (!SdMan.openFileForWrite("CPS", SETTINGS_FILE, outputFile)) {
    return false;
  }

  serialization::writePod(outputFile, SETTINGS_FILE_VERSION);
  serialization::writePod(outputFile, SETTINGS_COUNT);

#define CPS_WRITE_FIELD(name) serialization::writePod(outputFile, name);
  CPS_FIELDS(CPS_WRITE_FIELD)
#undef CPS_WRITE_FIELD

  outputFile.close();
  logf("[%lu] [CPS] Settings saved to file\n", millis());
  return true;
}

bool CrossPointSettings::loadFromFile() {
  FsFile inputFile;
  if (!SdMan.openFileForRead("CPS", SETTINGS_FILE, inputFile)) {
    return false;
  }

  uint8_t version = 0;
  serialization::readPod(inputFile, version);
  if (version != SETTINGS_FILE_VERSION) {
    logf("[%lu] [CPS] Deserialization failed: Unknown version %u\n", millis(), version);
    inputFile.close();
    return false;
  }

  uint8_t fileSettingsCount = 0;
  serialization::readPod(inputFile, fileSettingsCount);

  // Support older files (fewer fields) and tolerate newer files (more fields).
  const uint8_t toRead = (fileSettingsCount < SETTINGS_COUNT) ? fileSettingsCount : SETTINGS_COUNT;
  uint8_t settingsRead = 0;

#define CPS_READ_FIELD(name)              \
  do {                                    \
    if (!readIfAvailable(inputFile, toRead, settingsRead, name)) goto done; \
  } while (0);

  CPS_FIELDS(CPS_READ_FIELD)
#undef CPS_READ_FIELD

done:
  inputFile.close();
  logf("[%lu] [CPS] Settings loaded from file\n", millis());
  return true;
}

float CrossPointSettings::getReaderLineCompression() const {
  switch (fontFamily) {
    case NOTOSANS:
    case OPENDYSLEXIC:
      return compressionFor(lineSpacing, 0.90f, 0.95f, 1.0f);
    case BOOKERLY:
    default:
      return compressionFor(lineSpacing, 0.95f, 1.0f, 1.1f);
  }
}

unsigned long CrossPointSettings::getSleepTimeoutMs() const {
  switch (sleepTimeout) {
    case SLEEP_1_MIN:
      return minutesToMs(1);
    case SLEEP_5_MIN:
      return minutesToMs(5);
    case SLEEP_15_MIN:
      return minutesToMs(15);
    case SLEEP_30_MIN:
      return minutesToMs(30);
    case SLEEP_10_MIN:
    default:
      return minutesToMs(10);
  }
}

int CrossPointSettings::getRefreshFrequency() const {
  switch (refreshFrequency) {
    case REFRESH_1:
      return 1;
    case REFRESH_5:
      return 5;
    case REFRESH_10:
      return 10;
    case REFRESH_30:
      return 30;
    case REFRESH_15:
    default:
      return 15;
  }
}

int CrossPointSettings::getReaderFontId() const {
  switch (fontFamily) {
    case NOTOSANS:
      return fontIdForSize(fontSize, NOTOSANS_12_FONT_ID, NOTOSANS_14_FONT_ID, NOTOSANS_16_FONT_ID,
                           NOTOSANS_18_FONT_ID);

    case OPENDYSLEXIC:
      return fontIdForSize(fontSize, OPENDYSLEXIC_8_FONT_ID, OPENDYSLEXIC_10_FONT_ID, OPENDYSLEXIC_12_FONT_ID,
                           OPENDYSLEXIC_14_FONT_ID);

    case BOOKERLY:
    default:
      return fontIdForSize(fontSize, BOOKERLY_12_FONT_ID, BOOKERLY_14_FONT_ID, BOOKERLY_16_FONT_ID,
                           BOOKERLY_18_FONT_ID);
  }
}
