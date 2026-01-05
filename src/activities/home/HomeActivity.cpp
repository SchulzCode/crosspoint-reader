#include "HomeActivity.h"

#include <Arduino.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <SDCardManager.h>

#include <cstring>
#include <string>
#include <vector>

#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ScreenComponents.h"
#include "fontIds.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {

static bool endsWith(const std::string& s, const char* suffix) {
  const size_t sl = s.size();
  const size_t su = std::strlen(suffix);
  return (sl >= su) && (s.compare(sl - su, su, suffix) == 0);
}

static std::string baseName(const std::string& path) {
  const size_t lastSlash = path.find_last_of('/');
  return (lastSlash == std::string::npos) ? path : path.substr(lastSlash + 1);
}

static void stripKnownExt(std::string& name) {
  if (endsWith(name, ".xtch")) {
    name.resize(name.size() - 5);
  } else if (endsWith(name, ".xtc")) {
    name.resize(name.size() - 4);
  }
}

static void trimToWidthWithEllipsis(const GfxRenderer& r, const int fontId, std::string& s, const int maxWidth) {
  if (s.empty()) return;

  if (r.getTextWidth(fontId, s.c_str()) <= maxWidth) return;

  // Ensure space for "..."
  const char* ell = "...";
  while (!s.empty() && r.getTextWidth(fontId, (s + ell).c_str()) > maxWidth) {
    s.pop_back();
  }
  if (!s.empty()) s += ell;
}

static std::vector<std::string> wrapTextLines(const GfxRenderer& r,
                                              const int fontId,
                                              const std::string& text,
                                              const int maxWidth,
                                              const int maxLines) {
  std::vector<std::string> lines;
  lines.reserve(static_cast<size_t>(maxLines));

  const int spaceWidth = r.getSpaceWidth(fontId);

  std::string current;
  int currentWidth = 0;

  auto pushLine = [&](bool forceEllipsis) {
    if (current.empty()) return;

    if (forceEllipsis) {
      trimToWidthWithEllipsis(r, fontId, current, maxWidth);
      if (!endsWith(current, "...")) current += "...";
      trimToWidthWithEllipsis(r, fontId, current, maxWidth);
    }

    lines.push_back(current);
    current.clear();
    currentWidth = 0;
  };

  // Manual word scan (no stringstream)
  size_t pos = 0;
  while (pos < text.size()) {
    while (pos < text.size() && text[pos] == ' ') ++pos;
    if (pos >= text.size()) break;

    const size_t start = pos;
    while (pos < text.size() && text[pos] != ' ') ++pos;

    std::string word = text.substr(start, pos - start);

    // If a single word is too wide, trim it.
    while (word.size() > 5 && r.getTextWidth(fontId, word.c_str()) > maxWidth) {
      word.resize(word.size() - 4);
      word += "...";
    }

    const int wordWidth = r.getTextWidth(fontId, word.c_str());
    const int needed = (currentWidth == 0) ? wordWidth : (currentWidth + spaceWidth + wordWidth);

    if (needed > maxWidth && currentWidth != 0) {
      // Start a new line
      if (static_cast<int>(lines.size()) == maxLines - 1) {
        // Last line: append ellipsis and stop
        pushLine(true);
        return lines;
      }
      pushLine(false);
    }

    if (current.empty()) {
      current = word;
      currentWidth = wordWidth;
    } else {
      current += ' ';
      current += word;
      currentWidth = needed;
    }

    if (static_cast<int>(lines.size()) >= maxLines) {
      return lines;
    }
  }

  if (!current.empty() && static_cast<int>(lines.size()) < maxLines) {
    pushLine(false);
  }

  return lines;
}

static void notifyRender(TaskHandle_t h) {
  if (h) {
    xTaskNotifyGive(h);
  }
}

}  // namespace

void HomeActivity::taskTrampoline(void* param) {
  auto* self = static_cast<HomeActivity*>(param);
  self->displayTaskLoop();
}

int HomeActivity::getMenuItemCount() const { return hasContinueReading ? 4 : 3; }

void HomeActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  // Reset state
  selectorIndex = 0;
  lastBookTitle.clear();
  lastBookAuthor.clear();

  hasContinueReading = !APP_STATE.openEpubPath.empty() && SdMan.exists(APP_STATE.openEpubPath.c_str());

  if (hasContinueReading) {
    // Default to filename-based title
    lastBookTitle = baseName(APP_STATE.openEpubPath);

    // Prefer metadata for epub
    if (endsWith(APP_STATE.openEpubPath, ".epub")) {
      Epub epub(APP_STATE.openEpubPath, "/.crosspoint");
      epub.load(false);
      if (!epub.getTitle().empty()) lastBookTitle = std::string(epub.getTitle());
      if (!epub.getAuthor().empty()) lastBookAuthor = std::string(epub.getAuthor());
    } else {
      stripKnownExt(lastBookTitle);
    }
  }

  // Create render task
  xTaskCreate(&HomeActivity::taskTrampoline,
              "HomeActivityTask",
              2048,
              this,
              1,
              &displayTaskHandle);

  // First render
  notifyRender(displayTaskHandle);
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Ensure we don't kill the task mid-render / mid-EPD
  if (renderingMutex) {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
  }

  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }

  if (renderingMutex) {
    xSemaphoreGive(renderingMutex);
    vSemaphoreDelete(renderingMutex);
    renderingMutex = nullptr;
  }
}

void HomeActivity::loop() {
  const bool prevPressed = mappedInput.wasPressed(MappedInputManager::Button::Up) ||
                           mappedInput.wasPressed(MappedInputManager::Button::Left);
  const bool nextPressed = mappedInput.wasPressed(MappedInputManager::Button::Down) ||
                           mappedInput.wasPressed(MappedInputManager::Button::Right);

  const int menuCount = getMenuItemCount();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (hasContinueReading && selectorIndex == 0) {
      onContinueReading();
      return;
    }

    const int base = hasContinueReading ? 1 : 0;
    const int idx = selectorIndex - base;

    if (idx == 0) onReaderOpen();
    else if (idx == 1) onFileTransferOpen();
    else if (idx == 2) onSettingsOpen();
    return;
  }

  if (prevPressed) {
    selectorIndex = (selectorIndex + menuCount - 1) % menuCount;
    notifyRender(displayTaskHandle);
  } else if (nextPressed) {
    selectorIndex = (selectorIndex + 1) % menuCount;
    notifyRender(displayTaskHandle);
  }
}

void HomeActivity::displayTaskLoop() {
  for (;;) {
    // Block until loop() signals a render (binary/counting-semaphore style). :contentReference[oaicite:2]{index=2}
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    if (!renderingMutex) continue;
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    render();
    xSemaphoreGive(renderingMutex);
  }
}

void HomeActivity::render() const {
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  constexpr int margin = 20;
  constexpr int bottomMargin = 60;

  // --- Top "book" card ---
  const int bookWidth = pageWidth / 2;
  const int bookHeight = pageHeight / 2;
  const int bookX = (pageWidth - bookWidth) / 2;
  constexpr int bookY = 30;

  const bool bookSelected = hasContinueReading && selectorIndex == 0;

  // Card outline/fill
  if (bookSelected) renderer.fillRect(bookX, bookY, bookWidth, bookHeight);
  else renderer.drawRect(bookX, bookY, bookWidth, bookHeight);

  // Bookmark icon
  const int bookmarkWidth = bookWidth / 8;
  const int bookmarkHeight = bookHeight / 5;
  const int bookmarkX = bookX + bookWidth - bookmarkWidth - 8;
  constexpr int bookmarkY = bookY + 1;

  renderer.fillRect(bookmarkX, bookmarkY, bookmarkWidth, bookmarkHeight, !bookSelected);

  const int notchHeight = bookmarkHeight / 2;
  for (int i = 0; i < notchHeight; ++i) {
    const int y = bookmarkY + bookmarkHeight - 1 - i;
    const int xStart = bookmarkX + i;
    const int w = bookmarkWidth - 2 * i;
    if (w <= 0) break;
    renderer.fillRect(xStart, y, w, 1, bookSelected);
  }

  if (hasContinueReading) {
    const int maxLineWidth = bookWidth - 40;

    std::vector<std::string> lines = wrapTextLines(renderer, UI_12_FONT_ID, lastBookTitle, maxLineWidth, 3);

    int totalTextHeight = renderer.getLineHeight(UI_12_FONT_ID) * static_cast<int>(lines.size());
    if (!lastBookAuthor.empty()) {
      totalTextHeight += renderer.getLineHeight(UI_10_FONT_ID) * 3 / 2;
    }

    int y = bookY + (bookHeight - totalTextHeight) / 2;

    for (const auto& line : lines) {
      renderer.drawCenteredText(UI_12_FONT_ID, y, line.c_str(), !bookSelected);
      y += renderer.getLineHeight(UI_12_FONT_ID);
    }

    if (!lastBookAuthor.empty()) {
      y += renderer.getLineHeight(UI_10_FONT_ID) / 2;
      std::string author = lastBookAuthor;
      trimToWidthWithEllipsis(renderer, UI_10_FONT_ID, author, maxLineWidth);
      renderer.drawCenteredText(UI_10_FONT_ID, y, author.c_str(), !bookSelected);
    }

    renderer.drawCenteredText(UI_10_FONT_ID,
                              bookY + bookHeight - renderer.getLineHeight(UI_10_FONT_ID) * 3 / 2,
                              "Continue Reading",
                              !bookSelected);
  } else {
    const int y = bookY + (bookHeight - renderer.getLineHeight(UI_12_FONT_ID) - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, y, "No open book");
    renderer.drawCenteredText(UI_10_FONT_ID, y + renderer.getLineHeight(UI_12_FONT_ID), "Start reading below");
  }

  // --- Bottom menu tiles ---
  static constexpr const char* items[3] = {"Browse files", "File transfer", "Settings"};

  const int menuTileWidth = pageWidth - 2 * margin;
  constexpr int menuTileHeight = 50;
  constexpr int menuSpacing = 10;
  constexpr int totalMenuHeight = 3 * menuTileHeight + 2 * menuSpacing;

  int menuStartY = bookY + bookHeight + 20;
  const int maxMenuStartY = pageHeight - bottomMargin - totalMenuHeight - margin;
  if (menuStartY > maxMenuStartY) menuStartY = maxMenuStartY;

  for (int i = 0; i < 3; ++i) {
    const int overallIndex = i + (getMenuItemCount() - 3);
    const int tileY = menuStartY + i * (menuTileHeight + menuSpacing);
    const bool selected = selectorIndex == overallIndex;

    if (selected) renderer.fillRect(margin, tileY, menuTileWidth, menuTileHeight);
    else renderer.drawRect(margin, tileY, menuTileWidth, menuTileHeight);

    const char* label = items[i];
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label);
    const int textX = margin + (menuTileWidth - textWidth) / 2;
    const int textY = tileY + (menuTileHeight - renderer.getLineHeight(UI_10_FONT_ID)) / 2;

    renderer.drawText(UI_10_FONT_ID, textX, textY, label, !selected);
  }

  const auto labels = mappedInput.mapLabels("", "Confirm", "Up", "Down");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  ScreenComponents::drawBattery(renderer, 20, pageHeight - 70);

  renderer.displayBuffer();
}
