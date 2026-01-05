#include "KeyboardEntryActivity.h"

#include "MappedInputManager.h"
#include "fontIds.h"

#include <Arduino.h>

#include <cstring>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// Keyboard layouts - lowercase
const char* const KeyboardEntryActivity::keyboard[NUM_ROWS] = {
    "`1234567890-=", "qwertyuiop[]\\", "asdfghjkl;'", "zxcvbnm,./",
    "^  _____<OK"  // ^ = shift, _ = space, < = backspace, OK = done (rendered as buttons)
};

// Keyboard layouts - uppercase/symbols
// Keep the bottom row the same length as the lowercase row to avoid any accidental indexing surprises.
const char* const KeyboardEntryActivity::keyboardShift[NUM_ROWS] = {
    "~!@#$%^&*()_+",
    "QWERTYUIOP{}|",
    "ASDFGHJKL:\"",
    "ZXCVBNM<>?",
    "^  _____<OK"
};

namespace {

static inline void requestRender(TaskHandle_t h) {
  if (h) {
    xTaskNotifyGive(h);
  }
}

// Trim from the left until text fits; prefix with "..." when trimming occurs.
static void trimLeftToWidth(const GfxRenderer& r, const int fontId, std::string& s, const int maxWidth) {
  if (maxWidth <= 0) {
    s.clear();
    return;
  }
  if (s.empty()) return;
  if (r.getTextWidth(fontId, s.c_str()) <= maxWidth) return;

  const std::string ell = "...";
  // Find earliest start index so that "..." + tail fits.
  for (size_t start = 0; start <= s.size(); ++start) {
    std::string candidate = ell + s.substr(start);  // safe: start <= size()
    if (r.getTextWidth(fontId, candidate.c_str()) <= maxWidth) {
      s.swap(candidate);
      return;
    }
  }

  // If nothing fits, keep a minimal marker.
  s = ell;
}

static inline int centeredLeftMargin(const int pageWidth, const int rowChars, const int keyWidth, const int keySpacing) {
  // width = N*keyWidth + (N-1)*keySpacing
  const int rowWidth = rowChars * keyWidth + (rowChars > 0 ? (rowChars - 1) * keySpacing : 0);
  const int m = (pageWidth - rowWidth) / 2;
  return (m < 0) ? 0 : m;
}

static inline int spanWidth(const int cols, const int keyWidth, const int keySpacing) {
  return cols * keyWidth + (cols > 0 ? (cols - 1) * keySpacing : 0);
}

}  // namespace

void KeyboardEntryActivity::taskTrampoline(void* param) {
  auto* self = static_cast<KeyboardEntryActivity*>(param);
  self->displayTaskLoop();
}

void KeyboardEntryActivity::displayTaskLoop() {
  for (;;) {
    // Block until someone requests a render. :contentReference[oaicite:3]{index=3}
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    if (!renderingMutex) continue;
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    render();
    xSemaphoreGive(renderingMutex);
  }
}

void KeyboardEntryActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  xTaskCreate(&KeyboardEntryActivity::taskTrampoline,
              "KeyboardEntryActivity",
              2048,
              this,
              1,
              &displayTaskHandle);

  requestRender(displayTaskHandle);
}

void KeyboardEntryActivity::onExit() {
  Activity::onExit();

  // Avoid killing the task mid-render / mid-EPD operations
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

int KeyboardEntryActivity::getRowLength(const int row) const {
  static const int lens[NUM_ROWS] = {13, 13, 11, 10, 10};
  if (row < 0 || row >= NUM_ROWS) return 0;
  return lens[row];
}

char KeyboardEntryActivity::getSelectedChar() const {
  if (selectedRow < 0 || selectedRow >= NUM_ROWS) return '\0';

  // Special row is handled by handleKeyPress() and rendered as buttons.
  if (selectedRow == SPECIAL_ROW) return '\0';

  const char* const* layout = shiftActive ? keyboardShift : keyboard;

  const int rowLen = getRowLength(selectedRow);
  if (selectedCol < 0 || selectedCol >= rowLen) return '\0';

  return layout[selectedRow][selectedCol];
}

void KeyboardEntryActivity::handleKeyPress() {
  // Special row (bottom row with shift, space, backspace, done)
  if (selectedRow == SPECIAL_ROW) {
    if (selectedCol >= SHIFT_COL && selectedCol < SPACE_COL) {
      shiftActive = !shiftActive;
      return;
    }

    if (selectedCol >= SPACE_COL && selectedCol < BACKSPACE_COL) {
      if (maxLength == 0 || text.length() < maxLength) {
        text += ' ';
      }
      return;
    }

    if (selectedCol >= BACKSPACE_COL && selectedCol < DONE_COL) {
      if (!text.empty()) {
        text.pop_back();
      }
      return;
    }

    if (selectedCol >= DONE_COL) {
      if (onComplete) {
        onComplete(text);
      }
      return;
    }
  }

  // Regular character
  const char c = getSelectedChar();
  if (c == '\0') return;

  if (maxLength == 0 || text.length() < maxLength) {
    text += c;

    // Auto-disable shift after typing a letter
    if (shiftActive && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) {
      shiftActive = false;
    }
  }
}

void KeyboardEntryActivity::loop() {
  bool changed = false;

  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    if (selectedRow > 0) {
      selectedRow--;
      const int maxCol = getRowLength(selectedRow) - 1;
      if (selectedCol > maxCol) selectedCol = maxCol;
      changed = true;
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    if (selectedRow < NUM_ROWS - 1) {
      selectedRow++;
      const int maxCol = getRowLength(selectedRow) - 1;
      if (selectedCol > maxCol) selectedCol = maxCol;
      changed = true;
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    if (selectedRow == SPECIAL_ROW) {
      if (selectedCol >= SPACE_COL && selectedCol < BACKSPACE_COL) {
        selectedCol = SHIFT_COL;
        changed = true;
      } else if (selectedCol >= BACKSPACE_COL && selectedCol < DONE_COL) {
        selectedCol = SPACE_COL;
        changed = true;
      } else if (selectedCol >= DONE_COL) {
        selectedCol = BACKSPACE_COL;
        changed = true;
      }
    } else {
      if (selectedCol > 0) {
        selectedCol--;
        changed = true;
      } else if (selectedRow > 0) {
        selectedRow--;
        selectedCol = getRowLength(selectedRow) - 1;
        changed = true;
      }
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    const int maxCol = getRowLength(selectedRow) - 1;

    if (selectedRow == SPECIAL_ROW) {
      if (selectedCol >= SHIFT_COL && selectedCol < SPACE_COL) {
        selectedCol = SPACE_COL;
        changed = true;
      } else if (selectedCol >= SPACE_COL && selectedCol < BACKSPACE_COL) {
        selectedCol = BACKSPACE_COL;
        changed = true;
      } else if (selectedCol >= BACKSPACE_COL && selectedCol < DONE_COL) {
        selectedCol = DONE_COL;
        changed = true;
      }
    } else {
      if (selectedCol < maxCol) {
        selectedCol++;
        changed = true;
      } else if (selectedRow < NUM_ROWS - 1) {
        selectedRow++;
        selectedCol = 0;
        changed = true;
      }
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleKeyPress();
    changed = true;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (onCancel) {
      onCancel();
    }
    changed = true;
  }

  if (changed) {
    requestRender(displayTaskHandle);
  }
}

void KeyboardEntryActivity::render() const {
  const int pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();

  // Title
  renderer.drawCenteredText(UI_10_FONT_ID, startY, title.c_str());

  // Input field
  const int inputY = startY + 22;
  renderer.drawText(UI_10_FONT_ID, 10, inputY, "[");

  std::string displayText;
  if (isPassword) {
    displayText.assign(text.length(), '*');
  } else {
    displayText = text;
  }
  displayText += "_";  // cursor at end

  const int maxTextWidth = pageWidth - 40;  // inside brackets
  trimLeftToWidth(renderer, UI_10_FONT_ID, displayText, maxTextWidth);

  renderer.drawText(UI_10_FONT_ID, 20, inputY, displayText.c_str());
  renderer.drawText(UI_10_FONT_ID, pageWidth - 15, inputY, "]");

  // Keyboard
  const int keyboardStartY = inputY + 25;
  constexpr int keyWidth = 18;
  constexpr int keyHeight = 18;
  constexpr int keySpacing = 3;

  const char* const* layout = shiftActive ? keyboardShift : keyboard;

  // Center based on the longest (13-key) row.
  const int leftMargin = centeredLeftMargin(pageWidth, KEYS_PER_ROW, keyWidth, keySpacing);

  for (int row = 0; row < NUM_ROWS; ++row) {
    const int rowY = keyboardStartY + row * (keyHeight + keySpacing);
    const int startX = leftMargin;

    if (row == SPECIAL_ROW) {
      // Bottom row: CAPS(2) | SPACE(5) | <- (2) | OK(2)
      const int capsSpan = 2;
      const int spaceSpan = 5;
      const int bsSpan = 2;
      const int okSpan = 2;

      int x = startX;

      const bool capsSelected = (selectedRow == SPECIAL_ROW && selectedCol >= SHIFT_COL && selectedCol < SPACE_COL);
      renderItemWithSelector(x + 2, rowY, shiftActive ? "CAPS" : "caps", capsSelected);
      x += spanWidth(capsSpan, keyWidth, keySpacing) + keySpacing;

      const bool spaceSelected = (selectedRow == SPECIAL_ROW && selectedCol >= SPACE_COL && selectedCol < BACKSPACE_COL);
      const int spaceW = spanWidth(spaceSpan, keyWidth, keySpacing);
      const int spaceTextW = renderer.getTextWidth(UI_10_FONT_ID, "_____");
      const int spaceX = x + (spaceW - spaceTextW) / 2;
      renderItemWithSelector(spaceX, rowY, "_____", spaceSelected);
      x += spaceW + keySpacing;

      const bool bsSelected = (selectedRow == SPECIAL_ROW && selectedCol >= BACKSPACE_COL && selectedCol < DONE_COL);
      renderItemWithSelector(x + 2, rowY, "<-", bsSelected);
      x += spanWidth(bsSpan, keyWidth, keySpacing) + keySpacing;

      const bool okSelected = (selectedRow == SPECIAL_ROW && selectedCol >= DONE_COL);
      renderItemWithSelector(x + 2, rowY, "OK", okSelected);
    } else {
      const int rowLen = getRowLength(row);
      for (int col = 0; col < rowLen; ++col) {
        const char c = layout[row][col];
        char label[2] = {c, '\0'};

        const int charWidth = renderer.getTextWidth(UI_10_FONT_ID, label);
        const int keyX = startX + col * (keyWidth + keySpacing) + (keyWidth - charWidth) / 2;

        const bool isSelected = (row == selectedRow && col == selectedCol);
        renderItemWithSelector(keyX, rowY, label, isSelected);
      }
    }
  }

  // Hints
  const auto labels = mappedInput.mapLabels("« Back", "Select", "Left", "Right");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.drawSideButtonHints(UI_10_FONT_ID, "Up", "Down");

  renderer.displayBuffer();
}

void KeyboardEntryActivity::renderItemWithSelector(const int x, const int y, const char* item,
                                                   const bool isSelected) const {
  if (isSelected) {
    const int itemWidth = renderer.getTextWidth(UI_10_FONT_ID, item);
    renderer.drawText(UI_10_FONT_ID, x - 6, y, "[");
    renderer.drawText(UI_10_FONT_ID, x + itemWidth, y, "]");
  }
  renderer.drawText(UI_10_FONT_ID, x, y, item);
}
