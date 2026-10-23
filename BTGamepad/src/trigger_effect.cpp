// trigger_effect.cpp - DualSense アダプティブトリガー効果の生成
//
// 形式は Nielk1 氏の TriggerEffectGenerator を参考にした。
#include "BTGamepad.h"

#include <math.h>

// ---------------------------------------------------------------------------
// アダプティブトリガー効果
// ---------------------------------------------------------------------------
static void putZones(PSTriggerEffect& e, uint16_t zones) {
  e.data[1] = zones & 0xFF;
  e.data[2] = (zones >> 8) & 0xFF;
}

static void putForces(PSTriggerEffect& e, uint32_t forces) {
  e.data[3] = forces & 0xFF;
  e.data[4] = (forces >> 8) & 0xFF;
  e.data[5] = (forces >> 16) & 0xFF;
  e.data[6] = (forces >> 24) & 0xFF;
}

PSTriggerEffect PSTriggerEffect::off() {
  PSTriggerEffect e = {};
  e.data[0] = 0x05;
  return e;
}

PSTriggerEffect PSTriggerEffect::raw(const uint8_t bytes[11]) {
  PSTriggerEffect e;
  memcpy(e.data, bytes, 11);
  return e;
}

PSTriggerEffect PSTriggerEffect::multiFeedback(const uint8_t strength[10]) {
  PSTriggerEffect e = {};
  uint16_t zones = 0;
  uint32_t forces = 0;
  for (int i = 0; i < 10; i++) {
    if (strength[i] == 0) continue;
    uint8_t f = (strength[i] > 8 ? 8 : strength[i]) - 1;
    forces |= (uint32_t)(f & 0x07) << (3 * i);
    zones |= 1u << i;
  }
  if (!zones) return off();
  e.data[0] = 0x21;
  putZones(e, zones);
  putForces(e, forces);
  return e;
}

PSTriggerEffect PSTriggerEffect::feedback(uint8_t position, uint8_t strength) {
  if (position > 9 || strength == 0) return off();
  uint8_t s[10] = {};
  for (int i = position; i < 10; i++) s[i] = strength;
  return multiFeedback(s);
}

PSTriggerEffect PSTriggerEffect::slopeFeedback(uint8_t start, uint8_t end, uint8_t startStrength,
                                               uint8_t endStrength) {
  if (start > 8 || end <= start || end > 9 || startStrength == 0 || endStrength == 0) return off();
  uint8_t s[10] = {};
  for (int i = start; i < 10; i++) {
    if (i <= end) {
      float t = (float)(i - start) / (end - start);
      s[i] = (uint8_t)lroundf(startStrength + (endStrength - startStrength) * t);
    } else {
      s[i] = endStrength;
    }
  }
  return multiFeedback(s);
}

PSTriggerEffect PSTriggerEffect::weapon(uint8_t start, uint8_t end, uint8_t strength) {
  if (start < 2 || start > 7 || end <= start || end > 8 || strength == 0) return off();
  PSTriggerEffect e = {};
  e.data[0] = 0x25;
  putZones(e, (1u << start) | (1u << end));
  e.data[3] = (strength > 8 ? 8 : strength) - 1;
  return e;
}

PSTriggerEffect PSTriggerEffect::multiVibration(uint8_t frequency, const uint8_t amplitude[10]) {
  if (frequency == 0) return off();
  PSTriggerEffect e = {};
  uint16_t zones = 0;
  uint32_t forces = 0;
  for (int i = 0; i < 10; i++) {
    if (amplitude[i] == 0) continue;
    uint8_t a = (amplitude[i] > 8 ? 8 : amplitude[i]) - 1;
    forces |= (uint32_t)(a & 0x07) << (3 * i);
    zones |= 1u << i;
  }
  if (!zones) return off();
  e.data[0] = 0x26;
  putZones(e, zones);
  putForces(e, forces);
  e.data[9] = frequency;
  return e;
}

PSTriggerEffect PSTriggerEffect::vibration(uint8_t position, uint8_t amplitude, uint8_t frequency) {
  if (position > 9 || amplitude == 0) return off();
  uint8_t a[10] = {};
  for (int i = position; i < 10; i++) a[i] = amplitude;
  return multiVibration(frequency, a);
}

PSTriggerEffect PSTriggerEffect::bow(uint8_t start, uint8_t end, uint8_t strength, uint8_t snapForce) {
  if (start > 8 || end <= start || end > 8 || strength == 0 || snapForce == 0) return off();
  PSTriggerEffect e = {};
  e.data[0] = 0x22;
  putZones(e, (1u << start) | (1u << end));
  uint16_t pair = (((strength > 8 ? 8 : strength) - 1) & 0x07) | ((((snapForce > 8 ? 8 : snapForce) - 1) & 0x07) << 3);
  e.data[3] = pair & 0xFF;
  e.data[4] = (pair >> 8) & 0xFF;
  return e;
}

PSTriggerEffect PSTriggerEffect::galloping(uint8_t start, uint8_t end, uint8_t firstFoot, uint8_t secondFoot,
                                           uint8_t frequency) {
  if (start > 8 || end <= start || end > 9 || firstFoot > 6 || secondFoot <= firstFoot || secondFoot > 7 ||
      frequency == 0)
    return off();
  PSTriggerEffect e = {};
  e.data[0] = 0x23;
  putZones(e, (1u << start) | (1u << end));
  e.data[3] = (secondFoot & 0x07) | ((firstFoot & 0x07) << 3);
  e.data[4] = frequency;
  return e;
}

PSTriggerEffect PSTriggerEffect::machine(uint8_t start, uint8_t end, uint8_t amplitudeA, uint8_t amplitudeB,
                                         uint8_t frequency, uint8_t period) {
  if (start > 8 || end <= start || end > 9 || amplitudeA > 7 || amplitudeB > 7 || frequency == 0) return off();
  PSTriggerEffect e = {};
  e.data[0] = 0x27;
  putZones(e, (1u << start) | (1u << end));
  e.data[3] = (amplitudeA & 0x07) | ((amplitudeB & 0x07) << 3);
  e.data[4] = frequency;
  e.data[5] = period;
  return e;
}
