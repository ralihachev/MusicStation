#include "input.h"

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "pins.h"

namespace
{

const uint8_t ACTION_PINS[ACT_COUNT] = {
    BTN_UP, BTN_DOWN, BTN_ENTER, BTN_BACK, BTN_MENU,
};

// Only navigation auto-repeats. A repeating ENTER would fire a selection
// several times from one long press, and a repeating BACK would walk the user
// out of the menu tree.
const bool ACTION_REPEATS[ACT_COUNT] = {
    true, true, false, false, false,
};

const char *const ACTION_NAMES[ACT_COUNT] = {
    "UP", "DOWN", "ENTER", "BACK", "MENU",
};

struct RawEdge
{
  uint8_t action;
  bool pressed;
  uint32_t at;
};

// Sized well above the longest plausible burst. A ring buffer holds SIZE - 1
// entries, and it has to survive a full waveform's worth of impatient presses
// across five buttons without dropping the release that ends a hold.
constexpr uint8_t RAW_QUEUE_SIZE = 32;
volatile RawEdge rawEdges[RAW_QUEUE_SIZE] = {};
volatile uint8_t rawWriteIndex = 0;
volatile uint8_t rawReadIndex = 0;
volatile uint32_t lastEdgeAt[ACT_COUNT] = {};
volatile bool isrHeld[ACT_COUNT] = {};
portMUX_TYPE inputMux = portMUX_INITIALIZER_UNLOCKED;

// Logical events produced by pumping the raw queue. One pump can emit several
// repeats at once when a waveform blocked the loop while a key was held.
constexpr uint8_t EVENT_QUEUE_SIZE = 32;
InputEvent events[EVENT_QUEUE_SIZE];
uint8_t eventWriteIndex = 0;
uint8_t eventReadIndex = 0;

uint32_t pressedAt[ACT_COUNT] = {};
uint32_t lastRepeatAt[ACT_COUNT] = {};
bool held[ACT_COUNT] = {};
bool holdReported[ACT_COUNT] = {};
uint32_t lastActivityAt = 0;

void IRAM_ATTR onButtonEdge(void *arg)
{
  uint8_t action = (uint8_t)(uintptr_t)arg;
  // esp_timer is the clock behind millis(), so edges stamped here compare
  // directly against timing done in loop(). The FreeRTOS tick count is offset
  // from millis() by the pre-scheduler boot time and must not be used.
  uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
  bool pressed = digitalRead(ACTION_PINS[action]) == LOW; // active low

  portENTER_CRITICAL_ISR(&inputMux);
  // Debounce per button, not globally: pressing two buttons at once is a
  // legitimate gesture and must not swallow the second edge.
  if (now - lastEdgeAt[action] >= BUTTON_DEBOUNCE_MS &&
      pressed != isrHeld[action])
  {
    lastEdgeAt[action] = now;
    isrHeld[action] = pressed;
    uint8_t next = (uint8_t)((rawWriteIndex + 1) % RAW_QUEUE_SIZE);
    if (next != rawReadIndex)
    {
      rawEdges[rawWriteIndex].action = action;
      rawEdges[rawWriteIndex].pressed = pressed;
      rawEdges[rawWriteIndex].at = now;
      rawWriteIndex = next;
    }
  }
  portEXIT_CRITICAL_ISR(&inputMux);
}

bool popRawEdge(RawEdge &edge)
{
  bool available = false;
  portENTER_CRITICAL(&inputMux);
  if (rawReadIndex != rawWriteIndex)
  {
    edge.action = rawEdges[rawReadIndex].action;
    edge.pressed = rawEdges[rawReadIndex].pressed;
    edge.at = rawEdges[rawReadIndex].at;
    rawReadIndex = (uint8_t)((rawReadIndex + 1) % RAW_QUEUE_SIZE);
    available = true;
  }
  portEXIT_CRITICAL(&inputMux);
  return available;
}

void pushEvent(Action action, EventKind kind, bool fast, uint32_t at)
{
  uint8_t next = (uint8_t)((eventWriteIndex + 1) % EVENT_QUEUE_SIZE);
  if (next == eventReadIndex)
    return; // full: drop the newest rather than overwrite unread history
  events[eventWriteIndex] = {action, kind, fast, at};
  eventWriteIndex = next;
}

// Turns raw edges and elapsed time into logical events.
void pump()
{
  RawEdge edge;
  while (popRawEdge(edge))
  {
    Action action = (Action)edge.action;
    lastActivityAt = edge.at;
    if (edge.pressed)
    {
      held[action] = true;
      pressedAt[action] = edge.at;
      lastRepeatAt[action] = edge.at;
      holdReported[action] = false;
      pushEvent(action, EV_PRESS, false, edge.at);
    }
    else
    {
      held[action] = false;
      pushEvent(action, EV_RELEASE, false, edge.at);
    }
  }

  uint32_t now = millis();
  for (uint8_t i = 0; i < ACT_COUNT; i++)
  {
    if (!held[i])
      continue;
    Action action = (Action)i;
    uint32_t heldFor = now - pressedAt[i];

    if (!ACTION_REPEATS[i])
    {
      // Non-repeating actions report crossing the long-press threshold once,
      // which is what carries "hold BACK to return to Now Playing".
      if (!holdReported[i] && heldFor >= LONG_PRESS_MS)
      {
        holdReported[i] = true;
        lastActivityAt = now;
        pushEvent(action, EV_HOLD, false, now);
      }
      continue;
    }

    if (heldFor < KEY_REPEAT_DELAY_MS)
      continue;

    bool fast = heldFor >= FAST_SCROLL_AFTER_MS;
    // A blocking refresh can leave several repeat intervals owed. Emit them
    // all, so holding a key scans the list at full speed while the panel
    // catches up afterwards, and bound the burst so one stall cannot flood
    // the queue.
    uint8_t emitted = 0;
    while (now - lastRepeatAt[i] >= KEY_REPEAT_RATE_MS && emitted < 16)
    {
      lastRepeatAt[i] += KEY_REPEAT_RATE_MS;
      lastActivityAt = now;
      pushEvent(action, EV_REPEAT, fast, now);
      emitted++;
    }
    if (now - lastRepeatAt[i] >= KEY_REPEAT_RATE_MS)
      lastRepeatAt[i] = now; // discard the remainder of a long stall
  }
}

} // namespace

void inputBegin()
{
  for (uint8_t i = 0; i < ACT_COUNT; i++)
  {
    pinMode(ACTION_PINS[i], INPUT_PULLUP);
    // Seed the ISR's held state from the pins so a button already down at boot
    // does not synthesise a press on its first release.
    isrHeld[i] = digitalRead(ACTION_PINS[i]) == LOW;
    held[i] = isrHeld[i];
    attachInterruptArg(digitalPinToInterrupt(ACTION_PINS[i]), onButtonEdge,
                       (void *)(uintptr_t)i, CHANGE);
  }
}

bool inputNext(InputEvent &event)
{
  if (eventReadIndex == eventWriteIndex)
    pump();
  if (eventReadIndex == eventWriteIndex)
    return false;
  event = events[eventReadIndex];
  eventReadIndex = (uint8_t)((eventReadIndex + 1) % EVENT_QUEUE_SIZE);
  return true;
}

bool inputHeld(Action action)
{
  return action < ACT_COUNT && held[action];
}

bool inputAnyHeld()
{
  for (uint8_t i = 0; i < ACT_COUNT; i++)
    if (held[i])
      return true;
  return false;
}

uint32_t inputLastActivityAt() { return lastActivityAt; }

bool inputSettled(uint32_t settleMs)
{
  return millis() - lastActivityAt >= settleMs;
}

const char *actionName(Action action)
{
  return action < ACT_COUNT ? ACTION_NAMES[action] : "?";
}

const char *eventKindName(EventKind kind)
{
  switch (kind)
  {
  case EV_PRESS:
    return "press";
  case EV_REPEAT:
    return "repeat";
  case EV_HOLD:
    return "hold";
  case EV_RELEASE:
    return "release";
  }
  return "?";
}
