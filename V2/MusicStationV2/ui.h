// Browsing and playback interface.
//
// Input mutates this module's state and never draws. The sketch draws once
// input has gone quiet, so a burst of presses costs one ~780 ms refresh rather
// than one each. Now Playing is home; the library is somewhere you go.
// See V2/UI_DESIGN.md.
#pragma once

#include <Arduino.h>

#include "input.h"
#include "panel.h"

// List geometry. Seven rows of 15 px under a 16 px header.
constexpr int16_t HEADER_BASELINE = 12;
constexpr int16_t HEADER_RULE_Y = 16;
constexpr int16_t ROW_TOP = 18;
constexpr int16_t ROW_HEIGHT = 15;
constexpr uint8_t VISIBLE_ROWS = 7;

// Now Playing geometry, carried over from V1. The right column starts at a
// byte-aligned x so SSD1680 partial windows are reliable: 104 is 13 bytes and
// 184 is 23.
constexpr int16_t RIGHT_X = 104;
constexpr int16_t RIGHT_W = 184;
constexpr int16_t LYRIC_TOP = 31;
constexpr int16_t LYRIC_HEIGHT = 66;
constexpr uint8_t LYRIC_ROWS = 3;
constexpr int16_t TRANSPORT_Y = 100;
constexpr int16_t TRANSPORT_H = 28;

void uiBegin();
void uiGoNowPlaying();

// Boot lands here: the library list, with Now Playing still beneath it so BACK
// behaves the same as it does everywhere else.
void uiStartInLibrary();

bool uiHandleEvent(const InputEvent &event);

// Whole-screen draw callback for panelDraw.
void uiDraw();

// Region draws, used while Now Playing so a lyric change does not drive the
// artwork again and a clock tick does not drive either.
void uiDrawLyricColumn();
void uiDrawTransport();
PanelWindow uiLyricWindow();
PanelWindow uiTransportWindow();

bool uiOnNowPlaying();

bool uiWantsFullRefresh();
void uiClearFullRefresh();

// Set when the user picks "Rescan card". The sketch performs the rebuild, so
// a blocking multi-second scan never runs inside an input handler.
bool uiTakeRescanRequest();

void uiBuildProgress(uint16_t tracksFound, const char *artist,
                     const char *album);
void uiShowMessage(const String &title, const String &detail);
