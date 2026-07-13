// C ports of Q Light Controller Plus RGB matrix scripts
// (resources/rgbscripts at qlcplus commit 25c41450), adapted for this rig:
// the 7 MOSFET channels are treated as a width x 1 pixel strip, and each
// script's packed-RGB pixel collapses to a PWM duty (0 = off,
// EFFECT_DUTY_MAX = full on, same 0-1023 range as main.cpp's PWM_MAX).
//
// Ported so far: onebyone.js (Jano Svitok), fill.js / fillunfill.js
// (Massimo Callegari), evenodd.js (Heikki Junnila), strobe.js
// (Rob Nieuwenhuizen). Originals are Apache License 2.0; these ports keep
// that license and attribution.
//
// Driven by main.cpp's effects test mode (serial key 'e'): each frame sets
// per-channel fade targets, and the fade engine ramps every channel toward
// its target -- locals via analogWrite, extender channels streamed over the
// [0xAC][channel][duty] link frame. Intermediate duty levels are fully
// supported end to end, so future ports of effects with trails/fades
// (starfield, verticalfall, ...) can emit any 0..EFFECT_DUTY_MAX value.
// The effects here all emit plain 0 or EFFECT_DUTY_MAX per channel.

#pragma once

#include <stdint.h>

static const uint16_t EFFECT_DUTY_MAX = 1023;

// Mirror of the QLC+ script API: rgbMapStepCount(width, height) and
// rgbMap(width, height, rgb, step), with the returned height x width RGB
// map flattened into a row-major duty array (duty[y * width + x]) and the
// user-chosen rgb color standing in for full duty. frame() must be called
// with step in [0, stepCount(width, height)) and duty sized width * height.
typedef struct {
  const char *name;
  uint16_t (*stepCount)(uint8_t width, uint8_t height);
  void (*frame)(uint8_t width, uint8_t height, uint16_t step, uint16_t duty[]);
} Effect;

// --- One By One (onebyone.js) -----------------------------------------
// All pixels dark except a single lit one scanning row-major; the script's
// xx = step % width, yy = step / width indexing collapses to duty[step].

static uint16_t oneByOneStepCount(uint8_t width, uint8_t height) {
  return (uint16_t)width * height;
}

static void oneByOneFrame(uint8_t width, uint8_t height, uint16_t step, uint16_t duty[]) {
  uint16_t count = (uint16_t)width * height;
  for (uint16_t i = 0; i < count; i++) {
    duty[i] = 0;
  }
  if (step < count) {
    duty[step] = EFFECT_DUTY_MAX;
  }
}

// --- Fill / Fill Reversed (fill.js) ------------------------------------
// Lights one more pixel per step until everything is on, then restarts.
// The script's four orientations reduce to two on a width x 1 strip:
// "Horizontal" (x <= step) and "Horizontal Reversed" (x >= width - step - 1);
// the vertical orientations are meaningless at height 1 and are dropped.

static uint16_t fillStepCount(uint8_t width, uint8_t height) {
  (void)height;
  return width;
}

static void fillFrameOriented(uint8_t width, uint8_t height, uint16_t step, uint16_t duty[], bool reversed) {
  for (uint8_t y = 0; y < height; y++) {
    for (uint8_t x = 0; x < width; x++) {
      bool lit = reversed ? (x >= (int)width - (int)step - 1) : (x <= step);
      duty[y * width + x] = lit ? EFFECT_DUTY_MAX : 0;
    }
  }
}

static void fillFrame(uint8_t width, uint8_t height, uint16_t step, uint16_t duty[]) {
  fillFrameOriented(width, height, step, duty, false);
}

static void fillReversedFrame(uint8_t width, uint8_t height, uint16_t step, uint16_t duty[]) {
  fillFrameOriented(width, height, step, duty, true);
}

// --- Fill Unfill (fillunfill.js) ---------------------------------------
// Fills left-to-right over the first width steps, then the lit region's
// tail chases out left-to-right over the remaining width - 1 steps.
// Horizontal orientation only, as with Fill above.

static uint16_t fillUnfillStepCount(uint8_t width, uint8_t height) {
  (void)height;
  return (uint16_t)width * 2 - 1;
}

static void fillUnfillFrame(uint8_t width, uint8_t height, uint16_t step, uint16_t duty[]) {
  for (uint8_t y = 0; y < height; y++) {
    for (uint8_t x = 0; x < width; x++) {
      bool lit = (step < width) ? (x <= step) : (x > (int)step - (int)width);
      duty[y * width + x] = lit ? EFFECT_DUTY_MAX : 0;
    }
  }
}

// --- Even/Odd (evenodd.js) ----------------------------------------------
// Two steps: even-indexed pixels on one step, odd-indexed on the other.
// The script advances its parity counter per pixel across rows, which is
// exactly the row-major linear index offset by step.

static uint16_t evenOddStepCount(uint8_t width, uint8_t height) {
  (void)width;
  (void)height;
  return 2;
}

static void evenOddFrame(uint8_t width, uint8_t height, uint16_t step, uint16_t duty[]) {
  uint16_t count = (uint16_t)width * height;
  for (uint16_t i = 0; i < count; i++) {
    duty[i] = ((step + i) % 2 == 0) ? EFFECT_DUTY_MAX : 0;
  }
}

// --- Strobe (strobe.js) --------------------------------------------------
// Everything on together on step 0, dark for the remaining steps of the
// cycle. effectStrobeFrequency mirrors the script's frequency property
// (range 2-10): higher = longer dark gap between flashes.

static uint16_t effectStrobeFrequency = 2;

static uint16_t strobeStepCount(uint8_t width, uint8_t height) {
  (void)width;
  (void)height;
  return effectStrobeFrequency;
}

static void strobeFrame(uint8_t width, uint8_t height, uint16_t step, uint16_t duty[]) {
  uint16_t level = (step % effectStrobeFrequency != 0) ? 0 : EFFECT_DUTY_MAX;
  uint16_t count = (uint16_t)width * height;
  for (uint16_t i = 0; i < count; i++) {
    duty[i] = level;
  }
}

// -------------------------------------------------------------------------

static const Effect EFFECTS[] = {
  { "One By One",    oneByOneStepCount,   oneByOneFrame },
  { "Fill",          fillStepCount,       fillFrame },
  { "Fill Reversed", fillStepCount,       fillReversedFrame },
  { "Fill Unfill",   fillUnfillStepCount, fillUnfillFrame },
  { "Even/Odd",      evenOddStepCount,    evenOddFrame },
  { "Strobe",        strobeStepCount,     strobeFrame },
};

static const uint8_t EFFECT_COUNT = sizeof(EFFECTS) / sizeof(EFFECTS[0]);
