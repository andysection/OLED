#include <Arduino.h>
#include <driver/gpio.h>

#ifndef OLED_POWERED_PROBE_ENABLED
#define OLED_POWERED_PROBE_ENABLED 0
#endif

static_assert(OLED_POWERED_PROBE_ENABLED == 0 ||
                  OLED_POWERED_PROBE_ENABLED == 1,
              "OLED_POWERED_PROBE_ENABLED must be 0 or 1");

namespace {

// AirM2M ESP32-C3 board header pins: 2, 3, 20, 28 and 27 respectively.
// Each OLED connection must include its own series resistor at the OLED pad.
constexpr uint8_t kPad9Gpio = 0;
constexpr uint8_t kPad10Gpio = 1;
constexpr uint8_t kPad11Gpio = 3;
constexpr uint8_t kPad12Gpio = 4;
constexpr uint8_t kPad14Gpio = 5;

constexpr uint8_t kSignalGpios[] = {
    kPad9Gpio, kPad10Gpio, kPad11Gpio, kPad12Gpio, kPad14Gpio,
};
constexpr uint8_t kSignalPads[] = {9, 10, 11, 12, 14};
constexpr size_t kSignalCount =
    sizeof(kSignalGpios) / sizeof(kSignalGpios[0]);

constexpr bool kPoweredProbeEnabled = OLED_POWERED_PROBE_ENABLED != 0;
constexpr uint32_t kHalfClockUs = 100;  // Approximately 5 kHz.
constexpr uint32_t kRiseTimeoutUs = 2000;
constexpr uint8_t kAttemptsPerAddress = 3;
constexpr uint8_t kAddresses[] = {0x3B, 0x3C, 0x3D, 0x3E};
constexpr size_t kAddressCount = sizeof(kAddresses) / sizeof(kAddresses[0]);
constexpr char kArmCommand[] = "ARM VDD15";
constexpr uint32_t kArmValidityMs = 30000;

struct Mapping {
  const char *name;
  uint8_t sdaPadA;
  uint8_t sdaGpioA;
  uint8_t sdaPadB;
  uint8_t sdaGpioB;
  uint8_t sclPad;
  uint8_t sclGpio;
  uint8_t resetPad;
  uint8_t resetGpio;
  uint8_t auxPad;
  uint8_t auxGpio;
};

// D2/D1/D0 are consecutive SSD1312 interface signals. These are the four
// plausible assignments for RES# and an auxiliary control. AUX may be an
// exposed SA0, an active-low CS#, or an unrelated pin if SA0 is fixed in ITO.
constexpr Mapping kMappings[] = {
    {"A1", 9, kPad9Gpio, 10, kPad10Gpio, 11, kPad11Gpio, 14,
     kPad14Gpio, 12, kPad12Gpio},
    {"A2", 9, kPad9Gpio, 10, kPad10Gpio, 11, kPad11Gpio, 12,
     kPad12Gpio, 14, kPad14Gpio},
    {"B1", 11, kPad11Gpio, 12, kPad12Gpio, 10, kPad10Gpio, 9,
     kPad9Gpio, 14, kPad14Gpio},
    {"B2", 11, kPad11Gpio, 12, kPad12Gpio, 10, kPad10Gpio, 14,
     kPad14Gpio, 9, kPad9Gpio},
};
constexpr size_t kMappingCount = sizeof(kMappings) / sizeof(kMappings[0]);

enum class ProbeResult : uint8_t {
  kNotRun,
  kNack,
  kAck,
  kSclStuckLow,
  kSdaIdleLow,
  kSdaOutActiveEarly,
  kAckNotReleased,
  kStopSclStuckLow,
  kStopSdaHeldLow,
};

enum class StopResult : uint8_t { kOk, kSclStuckLow, kSdaHeldLow };

enum class DirectionPattern : uint8_t {
  kNone,
  kSwitchableSa0,
  kFixed3c,
  kFixed3d,
  kAuxLow3c,
  kAuxLow3d,
  kAmbiguous,
};

struct AddressTally {
  uint8_t ackCount = 0;
  uint8_t nackCount = 0;
  ProbeResult fault = ProbeResult::kNotRun;
};

struct SweepResult {
  AddressTally addresses[kAddressCount];
  bool completed = false;
};

struct DirectionOutcome {
  DirectionOutcome() = default;
  DirectionOutcome(DirectionPattern patternValue, bool hasValidAckValue)
      : pattern(patternValue), hasValidAck(hasValidAckValue) {}

  DirectionPattern pattern = DirectionPattern::kNone;
  bool hasValidAck = false;
};

struct DirectionCandidate {
  DirectionCandidate() = default;
  DirectionCandidate(const Mapping *mappingValue, uint8_t sdaInPadValue,
                     uint8_t sdaInGpioValue, uint8_t sdaOutPadValue,
                     uint8_t sdaOutGpioValue,
                     DirectionPattern patternValue)
      : mapping(mappingValue),
        sdaInPad(sdaInPadValue),
        sdaInGpio(sdaInGpioValue),
        sdaOutPad(sdaOutPadValue),
        sdaOutGpio(sdaOutGpioValue),
        pattern(patternValue) {}

  const Mapping *mapping = nullptr;
  uint8_t sdaInPad = 0;
  uint8_t sdaInGpio = 0;
  uint8_t sdaOutPad = 0;
  uint8_t sdaOutGpio = 0;
  DirectionPattern pattern = DirectionPattern::kNone;
};

bool scanArmed = false;
uint32_t scanArmedAtMs = 0;

void disableInternalPulls(uint8_t gpio) {
  gpio_pullup_dis(static_cast<gpio_num_t>(gpio));
  gpio_pulldown_dis(static_cast<gpio_num_t>(gpio));
}

void releaseLine(uint8_t gpio) {
  digitalWrite(gpio, LOW);
  pinMode(gpio, INPUT);
  disableInternalPulls(gpio);
}

void driveLineLow(uint8_t gpio) {
  digitalWrite(gpio, LOW);
  pinMode(gpio, OUTPUT_OPEN_DRAIN);
  digitalWrite(gpio, LOW);
  disableInternalPulls(gpio);
}

void releaseAllLines() {
  for (uint8_t gpio : kSignalGpios) {
    releaseLine(gpio);
  }
}

void releaseBusLines(uint8_t sdaIn, uint8_t sdaOut, uint8_t scl) {
  releaseLine(sdaIn);
  releaseLine(sdaOut);
  releaseLine(scl);
}

bool waitForReleasedHigh(uint8_t gpio) {
  const uint32_t started = micros();
  while (digitalRead(gpio) == LOW) {
    if (micros() - started >= kRiseTimeoutUs) {
      return false;
    }
  }
  return true;
}

bool releaseAndWaitForHigh(uint8_t gpio) {
  releaseLine(gpio);
  return waitForReleasedHigh(gpio);
}

StopResult stopCondition(uint8_t sdaIn, uint8_t sdaOut, uint8_t scl) {
  driveLineLow(scl);
  driveLineLow(sdaIn);
  releaseLine(sdaOut);
  delayMicroseconds(kHalfClockUs);

  if (!releaseAndWaitForHigh(scl)) {
    releaseBusLines(sdaIn, sdaOut, scl);
    return StopResult::kSclStuckLow;
  }

  delayMicroseconds(kHalfClockUs);
  releaseLine(sdaIn);
  delayMicroseconds(kHalfClockUs);
  const bool released = digitalRead(sdaIn) == HIGH &&
                        digitalRead(sdaOut) == HIGH;
  releaseBusLines(sdaIn, sdaOut, scl);
  return released ? StopResult::kOk : StopResult::kSdaHeldLow;
}

ProbeResult stopAndReturn(uint8_t sdaIn, uint8_t sdaOut, uint8_t scl,
                          ProbeResult result) {
  switch (stopCondition(sdaIn, sdaOut, scl)) {
    case StopResult::kOk:
      return result;
    case StopResult::kSclStuckLow:
      return ProbeResult::kStopSclStuckLow;
    case StopResult::kSdaHeldLow:
    default:
      return ProbeResult::kStopSdaHeldLow;
  }
}

ProbeResult writeAddressAndReadAck(uint8_t address, uint8_t sdaIn,
                                   uint8_t sdaOut, uint8_t scl) {
  releaseLine(sdaIn);
  releaseLine(sdaOut);
  if (!releaseAndWaitForHigh(scl)) {
    releaseBusLines(sdaIn, sdaOut, scl);
    return ProbeResult::kSclStuckLow;
  }
  if (digitalRead(sdaIn) == LOW || digitalRead(sdaOut) == LOW) {
    releaseBusLines(sdaIn, sdaOut, scl);
    return ProbeResult::kSdaIdleLow;
  }

  delayMicroseconds(kHalfClockUs);
  driveLineLow(sdaIn);
  delayMicroseconds(kHalfClockUs);
  driveLineLow(scl);

  const uint8_t value = static_cast<uint8_t>(address << 1);  // Write address.
  for (int bit = 7; bit >= 0; --bit) {
    if ((value & (1U << bit)) != 0) {
      releaseLine(sdaIn);
    } else {
      driveLineLow(sdaIn);
    }
    delayMicroseconds(kHalfClockUs);

    if (!releaseAndWaitForHigh(scl)) {
      releaseBusLines(sdaIn, sdaOut, scl);
      return ProbeResult::kSclStuckLow;
    }
    delayMicroseconds(kHalfClockUs);

    // Split-SDA output must remain released throughout the eight address bits.
    if (digitalRead(sdaOut) == LOW) {
      driveLineLow(scl);
      return stopAndReturn(sdaIn, sdaOut, scl,
                           ProbeResult::kSdaOutActiveEarly);
    }
    driveLineLow(scl);
  }

  releaseLine(sdaIn);
  releaseLine(sdaOut);
  delayMicroseconds(kHalfClockUs);
  if (!releaseAndWaitForHigh(scl)) {
    releaseBusLines(sdaIn, sdaOut, scl);
    return ProbeResult::kSclStuckLow;
  }
  delayMicroseconds(kHalfClockUs);
  const bool acknowledged = digitalRead(sdaOut) == LOW;

  driveLineLow(scl);
  delayMicroseconds(kHalfClockUs);
  if (!waitForReleasedHigh(sdaOut)) {
    return stopAndReturn(sdaIn, sdaOut, scl,
                         ProbeResult::kAckNotReleased);
  }

  return stopAndReturn(sdaIn, sdaOut, scl,
                       acknowledged ? ProbeResult::kAck
                                    : ProbeResult::kNack);
}

const char *resultName(ProbeResult result) {
  switch (result) {
    case ProbeResult::kNack:
      return "NACK";
    case ProbeResult::kAck:
      return "ACK";
    case ProbeResult::kSclStuckLow:
      return "SCL_LOW";
    case ProbeResult::kSdaIdleLow:
      return "SDA_IDLE_LOW";
    case ProbeResult::kSdaOutActiveEarly:
      return "SDAOUT_EARLY_LOW";
    case ProbeResult::kAckNotReleased:
      return "ACK_HELD_LOW";
    case ProbeResult::kStopSclStuckLow:
      return "STOP_SCL_LOW";
    case ProbeResult::kStopSdaHeldLow:
      return "STOP_SDA_LOW";
    case ProbeResult::kNotRun:
    default:
      return "NOT_RUN";
  }
}

void setAuxLevel(const Mapping &mapping, bool releasedHigh) {
  if (releasedHigh) {
    releaseLine(mapping.auxGpio);
  } else {
    driveLineLow(mapping.auxGpio);
  }
}

void pulseReset(const Mapping &mapping) {
  driveLineLow(mapping.resetGpio);
  delay(2);
  releaseLine(mapping.resetGpio);
  delay(5);
}

void printIdleBusLevels(uint8_t sdaInPad, uint8_t sdaInGpio,
                        uint8_t sdaOutPad, uint8_t sdaOutGpio,
                        uint8_t sclPad, uint8_t sclGpio) {
  releaseBusLines(sdaInGpio, sdaOutGpio, sclGpio);
  delay(1);
  Serial.printf("SDAIN(P%u)=%c SDAOUT(P%u)=%c SCL(P%u)=%c", sdaInPad,
                digitalRead(sdaInGpio) == HIGH ? 'H' : 'L', sdaOutPad,
                digitalRead(sdaOutGpio) == HIGH ? 'H' : 'L', sclPad,
                digitalRead(sclGpio) == HIGH ? 'H' : 'L');
}

AddressTally probeAddress(uint8_t address, uint8_t sdaIn, uint8_t sdaOut,
                          uint8_t scl) {
  AddressTally tally;
  for (uint8_t attempt = 0; attempt < kAttemptsPerAddress; ++attempt) {
    const ProbeResult result =
        writeAddressAndReadAck(address, sdaIn, sdaOut, scl);
    if (result == ProbeResult::kAck) {
      ++tally.ackCount;
    } else if (result == ProbeResult::kNack) {
      ++tally.nackCount;
    } else {
      tally.fault = result;
      break;
    }
    delay(1);
  }
  return tally;
}

SweepResult probeSweep(uint8_t sdaIn, uint8_t sdaOut, uint8_t scl) {
  SweepResult sweep;
  sweep.completed = true;
  for (size_t index = 0; index < kAddressCount; ++index) {
    sweep.addresses[index] =
        probeAddress(kAddresses[index], sdaIn, sdaOut, scl);
    const AddressTally &tally = sweep.addresses[index];
    Serial.printf(" 0x%02X=", kAddresses[index]);
    if (tally.fault != ProbeResult::kNotRun) {
      Serial.print(resultName(tally.fault));
      sweep.completed = false;
      break;
    }
    Serial.printf("%u/%u", tally.ackCount, kAttemptsPerAddress);
  }
  Serial.println();
  return sweep;
}

bool isExactAddressPattern(const SweepResult &sweep,
                           uint8_t expectedAddress) {
  if (!sweep.completed) {
    return false;
  }
  for (size_t index = 0; index < kAddressCount; ++index) {
    const AddressTally &tally = sweep.addresses[index];
    if (tally.fault != ProbeResult::kNotRun) {
      return false;
    }
    if (kAddresses[index] == expectedAddress) {
      if (tally.ackCount != kAttemptsPerAddress || tally.nackCount != 0) {
        return false;
      }
    } else if (tally.nackCount != kAttemptsPerAddress ||
               tally.ackCount != 0) {
      return false;
    }
  }
  return true;
}

bool isAllNack(const SweepResult &sweep) {
  if (!sweep.completed) {
    return false;
  }
  for (size_t index = 0; index < kAddressCount; ++index) {
    const AddressTally &tally = sweep.addresses[index];
    if (tally.fault != ProbeResult::kNotRun || tally.ackCount != 0 ||
        tally.nackCount != kAttemptsPerAddress) {
      return false;
    }
  }
  return true;
}

bool isAllAck(const AddressTally &tally) {
  return tally.fault == ProbeResult::kNotRun &&
         tally.ackCount == kAttemptsPerAddress && tally.nackCount == 0;
}

bool isAllNack(const AddressTally &tally) {
  return tally.fault == ProbeResult::kNotRun && tally.ackCount == 0 &&
         tally.nackCount == kAttemptsPerAddress;
}

void printTally(const char *label, uint8_t address,
                const AddressTally &tally) {
  Serial.printf("  %s(0x%02X)=", label, address);
  if (tally.fault != ProbeResult::kNotRun) {
    Serial.println(resultName(tally.fault));
    return;
  }
  Serial.printf("%u/%u ACK\n", tally.ackCount, kAttemptsPerAddress);
}

const char *patternName(DirectionPattern pattern) {
  switch (pattern) {
    case DirectionPattern::kSwitchableSa0:
      return "SA0 switch: LOW->0x3C, RELEASED->0x3D";
    case DirectionPattern::kFixed3c:
      return "fixed 0x3C in both AUX states";
    case DirectionPattern::kFixed3d:
      return "fixed 0x3D in both AUX states";
    case DirectionPattern::kAuxLow3c:
      return "AUX low enables fixed 0x3C (CS# candidate)";
    case DirectionPattern::kAuxLow3d:
      return "AUX low enables fixed 0x3D (CS# candidate)";
    case DirectionPattern::kAmbiguous:
      return "valid ACK seen, control behavior unresolved";
    case DirectionPattern::kNone:
    default:
      return "no complete ACK signature";
  }
}

bool isConfirmablePattern(DirectionPattern pattern) {
  return pattern == DirectionPattern::kSwitchableSa0 ||
         pattern == DirectionPattern::kFixed3c ||
         pattern == DirectionPattern::kFixed3d ||
         pattern == DirectionPattern::kAuxLow3c ||
         pattern == DirectionPattern::kAuxLow3d;
}

DirectionOutcome classifyDirection(const SweepResult &lowSweep,
                                   const SweepResult &highSweep) {
  const bool low3c = isExactAddressPattern(lowSweep, 0x3C);
  const bool low3d = isExactAddressPattern(lowSweep, 0x3D);
  const bool high3c = isExactAddressPattern(highSweep, 0x3C);
  const bool high3d = isExactAddressPattern(highSweep, 0x3D);
  const bool highNack = isAllNack(highSweep);
  const bool hasValidAck = low3c || low3d || high3c || high3d;

  DirectionPattern pattern = DirectionPattern::kNone;
  if (low3c && high3d) {
    pattern = DirectionPattern::kSwitchableSa0;
  } else if (low3c && high3c) {
    pattern = DirectionPattern::kFixed3c;
  } else if (low3d && high3d) {
    pattern = DirectionPattern::kFixed3d;
  } else if (low3c && highNack) {
    pattern = DirectionPattern::kAuxLow3c;
  } else if (low3d && highNack) {
    pattern = DirectionPattern::kAuxLow3d;
  } else if (hasValidAck) {
    pattern = DirectionPattern::kAmbiguous;
  }
  return DirectionOutcome(pattern, hasValidAck);
}

DirectionOutcome probeDirection(const Mapping &mapping, uint8_t sdaInPad,
                                uint8_t sdaInGpio, uint8_t sdaOutPad,
                                uint8_t sdaOutGpio) {
  Serial.printf("  SDAIN=P%u SDAOUT=P%u: idle ", sdaInPad, sdaOutPad);
  printIdleBusLevels(sdaInPad, sdaInGpio, sdaOutPad, sdaOutGpio,
                     mapping.sclPad, mapping.sclGpio);
  Serial.println();

  setAuxLevel(mapping, false);
  pulseReset(mapping);
  Serial.print("    AUX=LOW     ");
  const SweepResult lowSweep =
      probeSweep(sdaInGpio, sdaOutGpio, mapping.sclGpio);

  setAuxLevel(mapping, true);
  pulseReset(mapping);
  Serial.print("    AUX=RELEASED");
  const SweepResult highSweep =
      probeSweep(sdaInGpio, sdaOutGpio, mapping.sclGpio);

  const DirectionOutcome outcome = classifyDirection(lowSweep, highSweep);
  Serial.printf("    direction verdict: %s\n", patternName(outcome.pattern));
  releaseAllLines();
  delay(5);
  return outcome;
}

void probeMapping(const Mapping &mapping, DirectionCandidate *matches,
                  size_t &matchCount, size_t matchCapacity,
                  size_t &validAckDirectionCount) {
  Serial.printf("\n%s: pair=P%u/P%u SCL=P%u HIGH-CTRL=P%u AUX=P%u\n",
                mapping.name, mapping.sdaPadA, mapping.sdaPadB,
                mapping.sclPad, mapping.resetPad, mapping.auxPad);

  const DirectionOutcome forward =
      probeDirection(mapping, mapping.sdaPadA, mapping.sdaGpioA,
                     mapping.sdaPadB, mapping.sdaGpioB);
  validAckDirectionCount += forward.hasValidAck ? 1 : 0;
  if (isConfirmablePattern(forward.pattern) && matchCount < matchCapacity) {
    matches[matchCount++] =
        DirectionCandidate(&mapping, mapping.sdaPadA, mapping.sdaGpioA,
                           mapping.sdaPadB, mapping.sdaGpioB,
                           forward.pattern);
  }

  const DirectionOutcome reverse =
      probeDirection(mapping, mapping.sdaPadB, mapping.sdaGpioB,
                     mapping.sdaPadA, mapping.sdaGpioA);
  validAckDirectionCount += reverse.hasValidAck ? 1 : 0;
  if (isConfirmablePattern(reverse.pattern) && matchCount < matchCapacity) {
    matches[matchCount++] =
        DirectionCandidate(&mapping, mapping.sdaPadB, mapping.sdaGpioB,
                           mapping.sdaPadA, mapping.sdaGpioA,
                           reverse.pattern);
  }
}

bool everyPadIsHigh() {
  releaseAllLines();
  delay(2);
  bool allHigh = true;
  Serial.print("Idle digital levels: ");
  for (size_t index = 0; index < kSignalCount; ++index) {
    const bool high = digitalRead(kSignalGpios[index]) == HIGH;
    Serial.printf("P%u=%c ", kSignalPads[index], high ? 'H' : 'L');
    allHigh = allHigh && high;
  }
  Serial.println();
  return allHigh;
}

bool verifyResetLikeControl(const DirectionCandidate &candidate) {
  const Mapping &mapping = *candidate.mapping;
  Serial.printf("\nRES#/required-high control check for %s, ", mapping.name);
  Serial.printf("SDAIN=P%u, SDAOUT=P%u:\n",
                candidate.sdaInPad, candidate.sdaOutPad);

  releaseAllLines();
  const bool auxMustBeLow =
      candidate.pattern == DirectionPattern::kAuxLow3c ||
      candidate.pattern == DirectionPattern::kAuxLow3d;
  const uint8_t workingAddress =
      candidate.pattern == DirectionPattern::kFixed3d ||
              candidate.pattern == DirectionPattern::kAuxLow3d ||
              candidate.pattern == DirectionPattern::kSwitchableSa0
          ? 0x3D
          : 0x3C;
  setAuxLevel(mapping, !auxMustBeLow);
  pulseReset(mapping);
  const AddressTally before = probeAddress(
      workingAddress, candidate.sdaInGpio, candidate.sdaOutGpio,
      mapping.sclGpio);

  driveLineLow(mapping.resetGpio);
  delay(2);
  const AddressTally held3c = probeAddress(
      0x3C, candidate.sdaInGpio, candidate.sdaOutGpio, mapping.sclGpio);
  const AddressTally held3d = probeAddress(
      0x3D, candidate.sdaInGpio, candidate.sdaOutGpio, mapping.sclGpio);

  releaseLine(mapping.resetGpio);
  delay(5);
  const AddressTally after = probeAddress(
      workingAddress, candidate.sdaInGpio, candidate.sdaOutGpio,
      mapping.sclGpio);
  releaseAllLines();

  Serial.printf("  pattern=%s\n", patternName(candidate.pattern));
  printTally("before", workingAddress, before);
  printTally("held", 0x3C, held3c);
  printTally("held", 0x3D, held3d);
  printTally("after", workingAddress, after);
  const bool exact = isAllAck(before) && isAllNack(held3c) &&
                     isAllNack(held3d) && isAllAck(after);
  Serial.printf("  required-high control verdict: %s\n",
                exact ? "3/3 ACK -> (0/3,0/3) -> 3/3 ACK"
                      : "not confirmed (faults are not NACKs)");
  return exact;
}

void runScan() {
  Serial.println("\nSCAN requested.");
  if (!kPoweredProbeEnabled) {
    Serial.println("LOCKED: this firmware was built with powered probing off.");
    Serial.println("Do not power OLED pin 15 until the passive tests in README");
    Serial.println("identify it as the VDD candidate.");
    scanArmed = false;
    return;
  }
  const bool armExpired =
      scanArmed && millis() - scanArmedAtMs > kArmValidityMs;
  if (!scanArmed || armExpired) {
    scanArmed = false;
    Serial.printf("ABORT: first type the exact command: %s\n", kArmCommand);
    Serial.println("The confirmation expires after 30 seconds.");
    return;
  }
  scanArmed = false;  // One scan per explicit confirmation.

  if (!everyPadIsHigh()) {
    Serial.println("ABORT: at least one externally pulled-up pad reads LOW.");
    Serial.println("Power off the bench supply and recheck wiring/voltages.");
    return;
  }

  Serial.println("Only address bytes and STOP conditions will be sent.");
  DirectionCandidate matches[kMappingCount * 2];
  size_t matchCount = 0;
  size_t validAckDirectionCount = 0;
  for (const Mapping &mapping : kMappings) {
    probeMapping(mapping, matches, matchCount,
                 sizeof(matches) / sizeof(matches[0]),
                 validAckDirectionCount);
  }

  releaseAllLines();
  Serial.printf("\nConfirmable control patterns: %u\n",
                static_cast<unsigned>(matchCount));
  if (matchCount == 1 && verifyResetLikeControl(matches[0])) {
    const DirectionCandidate &winner = matches[0];
    const Mapping &mapping = *winner.mapping;
    Serial.printf("STRONG BUS/CONTROL CANDIDATE: SDAIN=P%u SDAOUT=P%u SCL=P%u ",
                  winner.sdaInPad, winner.sdaOutPad, mapping.sclPad);
    Serial.printf("RES#/required-high-control=P%u ", mapping.resetPad);
    if (winner.pattern == DirectionPattern::kSwitchableSa0) {
      Serial.printf("SA0=P%u\n", mapping.auxPad);
    } else if (winner.pattern == DirectionPattern::kAuxLow3c ||
               winner.pattern == DirectionPattern::kAuxLow3d) {
      Serial.printf("AUX(active-low/CS# candidate)=P%u\n", mapping.auxPad);
    } else {
      Serial.printf("AUX=P%u unresolved; SA0 appears fixed\n", mapping.auxPad);
    }
  } else if (matchCount == 0) {
    if (validAckDirectionCount != 0) {
      Serial.printf("Valid I2C ACK seen in %u direction(s), but control ",
                    static_cast<unsigned>(validAckDirectionCount));
      Serial.println("behavior is ambiguous. Share the complete log.");
    } else {
      Serial.println("INCONCLUSIVE: no complete I2C ACK signature observed.");
      Serial.println("Possible causes include CS#/BS straps, RES#, SDA direction,");
      Serial.println("power conditions, or a non-I2C interface mode.");
    }
    Serial.println("Do not increase voltage or current.");
  } else if (matchCount > 1) {
    Serial.println("Ambiguous result; RES# check skipped.");
  } else {
    Serial.println("Control pattern matched, but RES#/HIGH control was not confirmed.");
  }
  releaseAllLines();
  Serial.println("Done. All five GPIOs are high impedance.");
}

void armScan() {
  if (!kPoweredProbeEnabled) {
    Serial.println("LOCKED: OLED_POWERED_PROBE_ENABLED is 0.");
    scanArmed = false;
    return;
  }
  scanArmed = true;
  scanArmedAtMs = millis();
  Serial.println("Armed for one scan. Recheck >=2.80 V, CV and no heating,");
  Serial.println("then type SCAN within 30 s. Reset cancels confirmation.");
}

void printInstructions() {
  Serial.println();
  Serial.println("OLED split-SDA, open-drain ACK probe");
  Serial.println("AirM2M header: GPIO0=P2 GPIO1=P3 GPIO3=P20");
  Serial.println("                GPIO4=P28 GPIO5=P27");
  Serial.println("OLED map: P9->GPIO0 P10->GPIO1 P11->GPIO3");
  Serial.println("          P12->GPIO4 P14->GPIO5");
  Serial.println("All five lines start and finish in high impedance.");
  if (kPoweredProbeEnabled) {
    Serial.println("Powered probing is ENABLED in this build.");
    Serial.printf("Commands: PINS, %s, SCAN\n", kArmCommand);
  } else {
    Serial.println("Powered probing is LOCKED in this build (safe default).");
    Serial.println("PINS is available; SCAN cannot drive any OLED line.");
  }
}

}  // namespace

void setup() {
  releaseAllLines();
  Serial.begin(115200);
  Serial.setTimeout(100);
  delay(1000);
  printInstructions();
}

void loop() {
  if (Serial.available() == 0) {
    delay(10);
    return;
  }

  String command = Serial.readStringUntil('\n');
  command.trim();
  command.toUpperCase();
  if (command == "SCAN") {
    runScan();
  } else if (command == "PINS") {
    everyPadIsHigh();
  } else if (command == kArmCommand) {
    armScan();
  } else if (command.length() != 0) {
    Serial.println("Unknown command. Use PINS; powered builds also use ARM VDD15 and SCAN.");
  }
}
