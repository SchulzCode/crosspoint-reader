#include "MappedInputManager.h"
#include "CrossPointSettings.h"

namespace {
using FrontLayout = CrossPointSettings::FRONT_BUTTON_LAYOUT;
using SideLayout  = CrossPointSettings::SIDE_BUTTON_LAYOUT;
using HwButton    = decltype(InputManager::BTN_BACK);

struct FrontMap {
  HwButton back;
  HwButton confirm;
  HwButton left;
  HwButton right;
};

inline FrontLayout getFrontLayout() {
  return static_cast<FrontLayout>(SETTINGS.frontButtonLayout);
}

inline SideLayout getSideLayout() {
  return static_cast<SideLayout>(SETTINGS.sideButtonLayout);
}

inline FrontMap frontMapFor(const FrontLayout layout) {
  switch (layout) {
    case CrossPointSettings::LEFT_RIGHT_BACK_CONFIRM:
      // Back->LEFT, Confirm->RIGHT, Left->BACK, Right->CONFIRM
      return {InputManager::BTN_LEFT, InputManager::BTN_RIGHT, InputManager::BTN_BACK, InputManager::BTN_CONFIRM};

    case CrossPointSettings::LEFT_BACK_CONFIRM_RIGHT:
      // Back->CONFIRM, Confirm->LEFT, Left->BACK, Right->RIGHT
      return {InputManager::BTN_CONFIRM, InputManager::BTN_LEFT, InputManager::BTN_BACK, InputManager::BTN_RIGHT};

    case CrossPointSettings::BACK_CONFIRM_LEFT_RIGHT:
    default:
      // identity
      return {InputManager::BTN_BACK, InputManager::BTN_CONFIRM, InputManager::BTN_LEFT, InputManager::BTN_RIGHT};
  }
}

struct SideMap {
  HwButton pageBack;
  HwButton pageForward;
};

inline SideMap sideMapFor(const SideLayout layout) {
  switch (layout) {
    case CrossPointSettings::NEXT_PREV:
      // PageBack is "next" (down), PageForward is "prev" (up)
      return {InputManager::BTN_DOWN, InputManager::BTN_UP};

    case CrossPointSettings::PREV_NEXT:
    default:
      return {InputManager::BTN_UP, InputManager::BTN_DOWN};
  }
}
}  // namespace

HwButton MappedInputManager::mapButton(const Button button) const {
  const FrontMap fm = frontMapFor(getFrontLayout());
  const SideMap  sm = sideMapFor(getSideLayout());

  switch (button) {
    case Button::Back:        return fm.back;
    case Button::Confirm:     return fm.confirm;
    case Button::Left:        return fm.left;
    case Button::Right:       return fm.right;

    case Button::Up:          return InputManager::BTN_UP;
    case Button::Down:        return InputManager::BTN_DOWN;
    case Button::Power:       return InputManager::BTN_POWER;

    case Button::PageBack:    return sm.pageBack;
    case Button::PageForward: return sm.pageForward;
  }

  return InputManager::BTN_BACK;
}

bool MappedInputManager::wasPressed(const Button button) const { return inputManager.wasPressed(mapButton(button)); }
bool MappedInputManager::wasReleased(const Button button) const { return inputManager.wasReleased(mapButton(button)); }
bool MappedInputManager::isPressed(const Button button) const { return inputManager.isPressed(mapButton(button)); }
bool MappedInputManager::wasAnyPressed() const { return inputManager.wasAnyPressed(); }
bool MappedInputManager::wasAnyReleased() const { return inputManager.wasAnyReleased(); }
unsigned long MappedInputManager::getHeldTime() const { return inputManager.getHeldTime(); }

MappedInputManager::Labels MappedInputManager::mapLabels(const char* back,
                                                         const char* confirm,
                                                         const char* previous,
                                                         const char* next) const {
  const auto layout = getFrontLayout();

  // Input order: {back, confirm, previous, next}
  const char* in[4] = {back, confirm, previous, next};

  // Output order depends on FRONT_BUTTON_LAYOUT (same as your original behavior)
  // LEFT_RIGHT_BACK_CONFIRM: {previous, next, back, confirm} => idx {2,3,0,1}
  // LEFT_BACK_CONFIRM_RIGHT: {previous, back, confirm, next} => idx {2,0,1,3}
  // BACK_CONFIRM_LEFT_RIGHT: {back, confirm, previous, next} => idx {0,1,2,3}
  uint8_t idx[4] = {0, 1, 2, 3};

  switch (layout) {
    case CrossPointSettings::LEFT_RIGHT_BACK_CONFIRM:
      idx[0] = 2; idx[1] = 3; idx[2] = 0; idx[3] = 1;
      break;

    case CrossPointSettings::LEFT_BACK_CONFIRM_RIGHT:
      idx[0] = 2; idx[1] = 0; idx[2] = 1; idx[3] = 3;
      break;

    case CrossPointSettings::BACK_CONFIRM_LEFT_RIGHT:
    default:
      break;
  }

  return {in[idx[0]], in[idx[1]], in[idx[2]], in[idx[3]]};
}
