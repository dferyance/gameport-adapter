// This file is part of Necroware's GamePort adapter firmware.
// Copyright (C) 2022 Necroware
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include "Buffer.h"
#include "DigitalPin.h"
#include "Joystick.h"
#include "Utilities.h"
#include <Arduino.h>
#include <stdlib.h>

namespace {

/// Map of an axis number to corresponding packet byte offset.
/// Analog values are always 8-bit values so each axis maps to a
/// single byte
///
/// Index 4 is an analog axis but it isn't in this list as it is
/// used for the hat position which is converted to a digital hat
/// position
constexpr int axisIndex[] = {0, 1, 6, 3};
constexpr size_t axisIndex_size = sizeof(axisIndex) / sizeof(axisIndex[0]);

/// The throttle is handled differently than the other axis
/// For one, it doesn't auto-center so it isn't right to assume it
/// starts off at center. There isn't a detent for center either.
/// Also because the axis is part of the Attack Throttle, it is safer
/// to make assumptions about the axis range instead of being all calibration.
constexpr int throttleAxis = 2;

// These are just the starting values for the throttle
// I saw a range of 200 on mine but I don't want these to
// be exactly what I saw due to tolerances. So I narrowed it
// and auto-calibration will handle widening it to
// the actual range.
constexpr uint8_t throttleRange = 150;
constexpr uint8_t throttleCenter = 126;
constexpr uint8_t throttleMin = throttleCenter - (throttleRange / 2);
constexpr uint8_t throttleMax = throttleCenter + (throttleRange / 2);

/// Every packet is 13 bytes in length
/// I've seen this no matter what I have hooked up to the
/// throttle so it seems to be a fixed-limit.
constexpr size_t packetSize = 13;

/// The logic here is similar to the AnalogAxis class but with
/// some changes for the digital values from the Attack Throttle
/// 1. removal of reversing the axis direction
/// 2. it doesn't +100 or -100 for the starting position. We get 8-bit
/// values so that is too wide of a range.
/// 3. Multiple ways of initializing, for handing of the throttle
/// not auto-centering.
class AnalogAxisState {
public:
  AnalogAxisState()
  : m_min()
  , m_max() {
  }

  void init(uint8_t startPos) {
    m_min = startPos;
    m_max = startPos;
  }

  void init(uint8_t min, uint8_t max) {
    m_min = min;
    m_max = max;
  }

  int update(uint8_t analogValue) {
    if (analogValue < m_min) {
      m_min = analogValue;
    } else if (analogValue > m_max) {
      m_max = analogValue;
    }

    return map(analogValue, m_min, m_max, 0, 1023);
  }

private:
  uint8_t m_min;
  uint8_t m_max;
};
} // namespace

/// The Thrustmaster Attack Throttle uses the all-digital DirectConnect protocol
/// It has an input port that any analog joystick can be plugged into. But it was
/// usually used with a flight stick and rudder pedals. Thrustmaster sold a kit
/// that included the throttle with the Top Gun analog joystick and the Elite Rudder Pedals.
///
/// DirectConnect supported multiple digital joysticks on the same port
/// The Linux driver supports up to 2. This code doesn't currently support
/// more than 1 as I only have one digital device to test with.
///
/// This assumes the analog joystick plugged in matches the thrustmaster
/// configuration for the Top-Gun joystick and Elite Rudder Pedals.
/// It is fine to use something different such as just the joystick
/// but there is Thrustmaster-specific handling of the joystick hat.
class ThrustmasterAttackThrottle : public Joystick {
public:
  /// Resets the joystick and tries to detect the model.
  bool init() override {
    uint8_t packet[packetSize];
    bool packetOk = false;

    // The trigger pin starts out high, this causes two edges
    // on the first readPacket which throws off the timing.
    // Force to low first, allow it to stablize, then start
    // the attempt.
    m_trigger.setLow();
    delay(100);

    // 10 times is excessive, it should always work 100%
    // but with how timing sensitive the code is, I wanted to
    // give it some tries.
    for (int i = 0; i < 10 && !packetOk; ++i) {
      packetOk = readPacket(packet);
      if (!packetOk) {
        delay(200);
      }
    }

    // found an ok packet, initialize the axis range state
    if (packetOk) {
      for (size_t axis = 0; axis < axisIndex_size; ++axis) {
        uint8_t analogValue = packet[axisIndex[axis]];

        // Use the throttle ranges for the throttle,
        // otherwise treat the initial value as the center.
        if (axis == throttleAxis) {
          m_axisStates[axis].init(throttleMin, throttleMax);
        } else {
          m_axisStates[axis].init(analogValue);
        }
      }
    }
    return packetOk;
  }

  bool update() override {
    uint8_t packet[packetSize];

    bool packetOk = readPacket(packet);
    if (packetOk) {
      updateState(packet);
    } else {
      // wait a bit between bad packets in case it is still trying to output data.
      delay(200);
    }
    delay(53);

    return packetOk;
  }

  const State &getState() const override {
    return m_state;
  }

  const Description &getDescription() const override {
    static Description desc{"Thrustmaster Attack Throttle", axisIndex_size, 10, 1};
    return desc;
  }

private:
  DigitalInput<GamePort<2>::pin, true> m_data;
  DigitalInput<GamePort<7>::pin, true> m_clock;
  DigitalOutput<GamePort<3>::pin> m_trigger;

  AnalogAxisState m_axisStates[axisIndex_size];
  State m_state{};

  void trigger() {
    m_trigger.setHigh();
    delayMicroseconds(87);
    m_trigger.setLow();
  }

  /// Reads a packet from the gameport
  ///
  /// The Thrustmaster protocol is essentually SPI over two button pins.
  /// I connected a logic analyizer up to the gameport while running a vintage
  /// PC with the Thrustmaster drivers. The trigger causes the joystick to return
  /// a packet. The packet is sent via a clock and a data line. The data only changes on
  /// clock high so I look for a falling edge. I did see a pin that appeared to be
  /// an enable line and another trigger pulse. I believe that is useful with more
  /// than 1 digital joystick chained together. I only have one to test so I ignored it.
  ///
  /// The Linux kernel code is pretty much useless. I found it easier to reverse engineer
  /// myself. There is minimal documentation, single-letter variable names and just
  /// a complicated structure. The protocol is so simple it was easier to capture
  /// and process myself.
  ///
  /// @param packet is a buffer that is filled with the packet data
  /// @returns true if the data read ok, false on an error
  bool readPacket(uint8_t *packet) {

    // I'm not sure if this optimization actually does anything on AVR.
    // My thought is locality of reference may help to get the core loop
    // to be more performant. This is memcpy'ed to packet at the end when timing
    // doesn't matter.
    uint8_t localPacket[packetSize];

    // Halt interrupts only during the timing-sensitve part of the code
    //
    // This code is *very* timing-sensitive. There was a time where it was
    // working reliably until I changed an unrelated part of the code. Not
    // sure if that impacted inlining or code layout or something but it was
    // enough to break the timing.
    // I think this version is reliable. It has been in practice
    // but I wouldn't rule out something else odd like this happening again.
    const InterruptStopper interruptStopper;
    {
      trigger();

      // the first clock has a 200us timeout, every following clock
      // has a much shorter timeout. I've seen the gap between the
      // trigger and the first clock be about 100us. So 200us should
      // be plenty of margin. Having a long timeout on the first bit
      // makes this simpler than having extra wait calls to account
      // for this gap.
      int timeout = 200;

      // each byte is shifted into this variable
      uint8_t shiftReg;

      // current location in filling in the packet
      uint8_t *buffer = localPacket;

      for (uint8_t byte = 0; byte < packetSize; ++byte) {
        shiftReg = 0;

        // check for the start bit, it should always be 1
        if (!m_clock.wait(Edge::falling, timeout)) {
          log("timeout clk");
          return false;
        }

        if (!m_data.get()) {
          // bad start bit
          return false;
        }

        // after the first bit in the packet, adjust the timeout.
        // A clock pulse is 12us. so this should be enough although
        // I don't know how precise the timeout is.
        timeout = 24;

        // the data portion of the byte
        for (uint8_t bit = 0; bit < 8; ++bit) {

          if (!m_clock.wait(Edge::falling, 200)) {
            return false;
          }

          // shift in from the left
          shiftReg = shiftReg >> 1 | m_data.get() << 7;
        }

        if (!m_clock.wait(Edge::falling, 200)) {
          return false;
        }

        if (m_data.get()) {
          // bad stop bit
          log("bad stop bit");
          return false;
        }
        *buffer++ = shiftReg;
      }

      // A simple check that the packet is complete and there is no more data
      // A more comprehensive check would require some extra delays and checking
      // the clock pin as well
      //
      // I'd like this to be a rising edge check instead of just true
      // but couldn't get it to reliably detect both the data going low
      // then high fast enough on the Arduino.
      if (!m_data.wait(true, 200)) {
        log("timeout data");
        return false;
      }
    }

    // End of the timing sensitive portion
    // so safe to copy to the true packet buffer
    memcpy(packet, localPacket, packetSize);

    // wait before sending the next request
    delay(54);

    return true;
  }

  /// Maps the hat's analog value to directions
  ///
  /// The top-gun joystick's hat is connected
  /// to resistor dividers (actually point-to-point soldered).
  ///
  /// I'm sure the resistors aren't precision so I looked
  /// at what values I got in the different positions and added
  /// some margin to that. Interestingly, they end up with about
  /// 0, 25, 75, 100.
  ///
  /// The values here are different from the ThrustMaster.h as
  /// they are based on what the Attack Throttle ADCs and sends
  /// back as an 8-bit number instead of Arduino's ADC.
  int mapHat(uint8_t analogValue) {
    // up, about 0
    if (analogValue < 10) {
      return 1;
    }

    // right, about 25
    if (analogValue < 35) {
      return 3;
    }

    // down, about 50
    if (analogValue < 60) {
      return 5;
    }

    // left, about 75
    if (analogValue < 85) {
      return 7;
    }

    // i'm seeing around 100 for middle, but this allows for any other
    // value to be middle.
    return 0;
  }

  /// Updates the joystick state structure based off of the
  /// contents of the latest packet
  /// @param packet the latest packet
  void updateState(uint8_t *packet) {

    // update each axis state which returns what usb-scaled
    // axis value to use
    for (size_t axis = 0; axis < axisIndex_size; ++axis) {
      m_state.axes[axis] = m_axisStates[axis].update(packet[axisIndex[axis]]);
    }

    // hats are returned in an analog axis
    m_state.hats[0] = mapHat(packet[4]);

    // Other joysticks and modes may be different but I found the
    // buttons triggered the following bits:
    // throttle index 5 - 1111 1100
    // joystick index 2 - 1111 0000
    // so this masks / shifts them to be the first 10 buttons
    uint16_t buttons = ((~packet[5] & 0xFC) << 2) | ((~packet[2] & 0xF0) >> 4);

    m_state.buttons = buttons;
  }
};
