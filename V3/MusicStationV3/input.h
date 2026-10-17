// Button input: interrupt capture, debounce, and logical actions.
//
// Two things shape this module. First, the main loop does not run during the
// panel's ~780 ms waveform, so presses MUST be captured by an interrupt and
// queued; polling in loop() silently drops most of them. Second, the firmware
// names logical actions rather than buttons, so moving to four buttons or a
// rotary encoder is a change to ACTION_PINS and nothing else.
#pragma once

#include <Arduino.h>

enum Action : uint8_t
{
  ACT_UP = 0,
  ACT_DOWN,
  ACT_ENTER,
  ACT_BACK,
  ACT_MENU,
  ACT_COUNT
};

enum EventKind : uint8_t
{
  EV_PRESS,   // button went down
  EV_REPEAT,  // auto-repeat while held, navigation actions only
  EV_HOLD,    // long-press threshold reached, non-repeating actions only
  EV_RELEASE, // button came back up
};

struct InputEvent
{
  Action action;
  EventKind kind;
  bool fast;   // held long enough that navigation should move by a page
  uint32_t at; // millis timebase, stamped when the edge actually occurred
};

// Timing. SETTLE is not used here; it belongs to the renderer, which waits for
// input to go quiet before drawing so a burst of presses costs one refresh.
constexpr uint32_t BUTTON_DEBOUNCE_MS = 80;
constexpr uint32_t KEY_REPEAT_DELAY_MS = 450;
constexpr uint32_t KEY_REPEAT_RATE_MS = 90;
constexpr uint32_t FAST_SCROLL_AFTER_MS = 1500;
constexpr uint32_t LONG_PRESS_MS = 600;
constexpr uint32_t SCROLL_SETTLE_MS = 150;

void inputBegin();

// Drains one logical event. Call until it returns false.
bool inputNext(InputEvent &event);

bool inputHeld(Action action);
bool inputAnyHeld();

// Timestamp of the most recent edge, for the renderer's settle timer.
uint32_t inputLastActivityAt();
bool inputSettled(uint32_t settleMs = SCROLL_SETTLE_MS);

const char *actionName(Action action);
const char *eventKindName(EventKind kind);
