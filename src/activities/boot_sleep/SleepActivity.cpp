#include "SleepActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <SDCardManager.h>
#include <Xtc.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "fontIds.h"
#include "images/CrossLarge.h"

namespace {

static bool endsWith(const std::string& s, const char* suffix) {
  const size_t sl = s.size();
  const size_t su = std::strlen(suffix);
  return (sl >= su) && (s.compare(sl - su, su, suffix) == 0);
}

static bool isHiddenName(const std::string& name) { return !name.empty() && name[0] == '.'; }

static bool isXtcFile(const std::string& path) { return endsWith(path, ".xtc") || endsWith(path, ".xtch"); }

template <typename BookT>
static bool generateCoverBmpFor(const std::string& bookPath, std::string& outBmpPath, const char* kindTag) {
  BookT book(bookPath, "/.crosspoint");

  if (!book.load()) {
    Serial.printf("[SLP] Failed to load %s\n", kindTag);
    return false;
  }
  if (!book.generateCoverBmp()) {
    Serial.printf("[SLP] Failed to generate %s cover bmp\n", kindTag);
    return false;
  }

  outBmpPath = book.getCoverBmpPath();
  return true;
}

static bool collectValidBmpsInSleepDir(std::vector<std::string>& outFiles) {
  auto dir = SdMan.open("/sleep");
  if (!(dir && dir.isDirectory())) {
    if (dir) dir.close();
    return false;
  }

  char name[128];

  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) {
      file.close();
      continue;
    }

    file.getName(name, sizeof(name));
    const std::string filename(name);

    if (isHiddenName(filename)) {
      file.close();
      continue;
    }

    if (!endsWith(filename, ".bmp")) {
      Serial.printf("[%lu] [SLP] Skipping non-.bmp file name: %s\n", millis(), name);
      file.close();
      continue;
    }

    Bitmap bitmap(file);
    if (bitmap.parseHeaders() != BmpReaderError::Ok) {
      Serial.printf("[%lu] [SLP] Skipping invalid BMP file: %s\n", millis(), name);
      file.close();
      continue;
    }

    outFiles.push_back(filename);
    file.close();
  }

  dir.close();
  return !outFiles.empty();
}

static int clampNonNegative(int v) { return (v < 0) ? 0 : v; }

}  // namespace

bool SleepActivity::tryRenderBitmapPath(const char* path) const {
  FsFile file;
  if (!SdMan.openFileForRead("SLP", path, file)) {
    return false;
  }

  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    return false;
  }

  renderBitmapSleepScreen(bitmap);
  return true;
}

void SleepActivity::onEnter() {
  Activity::onEnter();
  renderPopup("Entering Sleep...");

  switch (SETTINGS.sleepScreen) {
    case CrossPointSettings::SLEEP_SCREEN_MODE::BLANK:
      return renderBlankSleepScreen();
    case CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM:
      return renderCustomSleepScreen();
    case CrossPointSettings::SLEEP_SCREEN_MODE::COVER:
      return renderCoverSleepScreen();
    default:
      break;
  }

  renderDefaultSleepScreen();
}

void SleepActivity::renderPopup(const char* message) const {
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, message, EpdFontFamily::BOLD);
  constexpr int margin = 20;
  const int x = (renderer.getScreenWidth() - textWidth - margin * 2) / 2;
  constexpr int y = 117;
  const int w = textWidth + margin * 2;
  const int h = renderer.getLineHeight(UI_12_FONT_ID) + margin * 2;

  renderer.fillRect(x - 5, y - 5, w + 10, h + 10, true);
  renderer.fillRect(x + 5, y + 5, w - 10, h - 10, false);
  renderer.drawText(UI_12_FONT_ID, x + margin, y + margin, message, true, EpdFontFamily::BOLD);
  renderer.displayBuffer();
}

void SleepActivity::renderCustomSleepScreen() const {
  // Prefer /sleep/*.bmp (random valid file).
  std::vector<std::string> files;
  if (collectValidBmpsInSleepDir(files)) {
    const size_t idx = static_cast<size_t>(random(static_cast<long>(files.size())));
    const std::string fullPath = std::string("/sleep/") + files[idx];

    Serial.printf("[%lu] [SLP] Randomly loading: %s\n", millis(), fullPath.c_str());
    delay(100);

    if (tryRenderBitmapPath(fullPath.c_str())) {
      return;
    }
  }

  // Fallback: /sleep.bmp at SD root.
  if (tryRenderBitmapPath("/sleep.bmp")) {
    Serial.printf("[%lu] [SLP] Loading: /sleep.bmp\n", millis());
    return;
  }

  renderDefaultSleepScreen();
}

void SleepActivity::renderDefaultSleepScreen() const {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  const int imgW = 128;
  const int imgH = 128;
  const int x = clampNonNegative((pageWidth - imgW) / 2);
  const int y = clampNonNegative((pageHeight - imgH) / 2);

  renderer.drawImage(CrossLarge, x, y, imgW, imgH);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, "CrossPoint", true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, "SLEEPING");

  if (SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::LIGHT) {
    renderer.invertScreen();
  }

  renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
}

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap) const {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  const int bmpW = bitmap.getWidth();
  const int bmpH = bitmap.getHeight();

  int x = 0;
  int y = 0;
  float cropX = 0.0f;
  float cropY = 0.0f;

  Serial.printf("[%lu] [SLP] bitmap %d x %d, screen %d x %d\n", millis(), bmpW, bmpH, pageWidth, pageHeight);

  const bool needsScale = (bmpW > pageWidth) || (bmpH > pageHeight);
  const bool doCrop = (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP);

  if (needsScale) {
    float ratio = static_cast<float>(bmpW) / static_cast<float>(bmpH);
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    Serial.printf("[%lu] [SLP] bitmap ratio: %f, screen ratio: %f\n", millis(), ratio, screenRatio);

    if (ratio > screenRatio) {
      if (doCrop) {
        cropX = 1.0f - (screenRatio / ratio);
        Serial.printf("[%lu] [SLP] Cropping bitmap x: %f\n", millis(), cropX);
        ratio = (1.0f - cropX) * static_cast<float>(bmpW) / static_cast<float>(bmpH);
      }
      x = 0;
      y = static_cast<int>(std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2.0f));
      Serial.printf("[%lu] [SLP] Centering with ratio %f to y=%d\n", millis(), ratio, y);
    } else {
      if (doCrop) {
        cropY = 1.0f - (ratio / screenRatio);
        Serial.printf("[%lu] [SLP] Cropping bitmap y: %f\n", millis(), cropY);
        ratio = static_cast<float>(bmpW) / ((1.0f - cropY) * static_cast<float>(bmpH));
      }
      x = static_cast<int>(std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2.0f));
      y = 0;
      Serial.printf("[%lu] [SLP] Centering with ratio %f to x=%d\n", millis(), ratio, x);
    }
  } else {
    x = (pageWidth - bmpW) / 2;
    y = (pageHeight - bmpH) / 2;
  }

  Serial.printf("[%lu] [SLP] drawing to %d x %d\n", millis(), x, y);

  renderer.clearScreen();
  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
  renderer.displayBuffer(EInkDisplay::HALF_REFRESH);

  if (bitmap.hasGreyscale()) {
    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    renderer.copyGrayscaleLsbBuffers();

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }
}

void SleepActivity::renderCoverSleepScreen() const {
  if (APP_STATE.openEpubPath.empty()) {
    return renderDefaultSleepScreen();
  }

  std::string coverBmpPath;
  const bool ok = isXtcFile(APP_STATE.openEpubPath)
                      ? generateCoverBmpFor<Xtc>(APP_STATE.openEpubPath, coverBmpPath, "XTC")
                      : generateCoverBmpFor<Epub>(APP_STATE.openEpubPath, coverBmpPath, "epub");

  if (!ok) {
    return renderDefaultSleepScreen();
  }

  if (tryRenderBitmapPath(coverBmpPath.c_str())) {
    return;
  }

  renderDefaultSleepScreen();
}

void SleepActivity::renderBlankSleepScreen() const {
  renderer.clearScreen();
  renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
}
