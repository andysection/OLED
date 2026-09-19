#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_attr.h>
#include <esp_system.h>

#ifndef ADDRESS_ACK_TEST_BUILD
#define ADDRESS_ACK_TEST_BUILD 0
#endif

static_assert(ADDRESS_ACK_TEST_BUILD == 1,
              "Select the address_ack_test environment explicitly");

namespace {

// Fixed candidate mapping. P8 and P11 can never enter an output helper.
constexpr gpio_num_t kSdaOut = GPIO_NUM_0;  // OLED P8  / D2 / SDAOUT
constexpr gpio_num_t kSdaIn = GPIO_NUM_1;   // OLED P9  / D1 / SDAIN
constexpr gpio_num_t kScl = GPIO_NUM_3;     // OLED P10 / D0 / SCL
constexpr gpio_num_t kSa0 = GPIO_NUM_4;     // OLED P11 / D/C# / SA0
constexpr gpio_num_t kReset = GPIO_NUM_5;   // OLED P12 / RES#

constexpr gpio_num_t kSignalGpios[] = {
    kSdaOut, kSdaIn, kScl, kSa0, kReset,
};

constexpr uint64_t kAllSignalMask =
    (1ULL << kSdaOut) | (1ULL << kSdaIn) | (1ULL << kScl) |
    (1ULL << kSa0) | (1ULL << kReset);

// P11/SA0 is externally biased LOW. No other address exists in this build.
constexpr uint8_t kAddress7Bit = 0x3C;
constexpr uint8_t kAddressWriteByte =
    static_cast<uint8_t>(kAddress7Bit << 1);
static_assert(kAddressWriteByte == 0x78, "Unexpected address byte");
static_assert((kAddressWriteByte & 1U) == 0, "ACK test must be a write");

constexpr uint32_t kArmTimeoutMs = 15000;
constexpr uint32_t kMinimumConfirmDelayMs = 1000;
constexpr uint32_t kResetLowMs = 5000;
constexpr uint32_t kResetRecoveryMs = 100;
constexpr uint32_t kDataSetupUs = 500;
constexpr uint32_t kClockHighHoldUs = 500;
constexpr uint32_t kClockLowHoldUs = 500;
constexpr uint32_t kLineRiseTimeoutUs = 5000;
constexpr uint32_t kLineHighSettleUs = 250;
constexpr uint32_t kAckSampleGapUs = 300;
constexpr uint32_t kTransactionLimitUs = 50000;

enum class PersistentState : uint32_t {
  kNeedReset = 0,
  kResetInProgress = 1,
  kResetQualified = 2,
  kAckInProgress = 3,
  kDone = 4,
  kFault = 5,
};

enum class ArmedAction : uint8_t {
  kNone,
  kReset,
  kAck,
};

struct GuardCookie {
  uint32_t magic;
  uint32_t state;
  uint32_t invertedState;
};

constexpr uint32_t kGuardMagic = 0x4F4C4544U;  // "OLED"
RTC_NOINIT_ATTR GuardCookie gGuardCookie;

PersistentState persistentState = PersistentState::kFault;
ArmedAction armedAction = ArmedAction::kNone;
bool faultLatched = false;
bool bootCookieInvalid = false;
uint32_t armStartedMs = 0;
uint32_t armDeadlineMs = 0;

struct Levels {
  int p8;
  int p9;
  int p10;
  int p11;
  int p12;
};

struct StableLevels {
  Levels levels;
  bool stable;
};

struct ReleaseReport {
  esp_err_t combinedResult;
  bool fallbackUsed;
  esp_err_t fallbackResults[5];
  bool configurationSucceeded;
  Levels levels;
  bool levelsExpected;
  bool verified;
};

struct ResetReport {
  StableLevels initial;
  int assertedLevel;
  int releasedLevel;
  const char *failureStage;
  ReleaseReport release;
};

struct AckReport {
  StableLevels initial;
  uint8_t bitsCompleted;
  bool startMayHaveOccurred;
  bool startIssued;
  int ackP8Sample1;
  int ackP8Sample2;
  int ackP9Sample1;
  int ackP9Sample2;
  bool stopAttempted;
  bool stopCompleted;
  const char *failureStage;
  const char *stopFailureStage;
  ReleaseReport release;
};

Levels readLevels() {
  return {
      gpio_get_level(kSdaOut),
      gpio_get_level(kSdaIn),
      gpio_get_level(kScl),
      gpio_get_level(kSa0),
      gpio_get_level(kReset),
  };
}

bool levelsEqual(const Levels &a, const Levels &b) {
  return a.p8 == b.p8 && a.p9 == b.p9 && a.p10 == b.p10 &&
         a.p11 == b.p11 && a.p12 == b.p12;
}

StableLevels readStableLevels() {
  const Levels first = readLevels();
  delayMicroseconds(500);
  const Levels second = readLevels();
  delayMicroseconds(500);
  const Levels third = readLevels();
  return {third, levelsEqual(first, second) && levelsEqual(second, third)};
}

bool levelsMatchFixedWiring(const Levels &levels) {
  return levels.p8 != 0 && levels.p9 != 0 && levels.p10 != 0 &&
         levels.p11 == 0 && levels.p12 != 0;
}

esp_err_t makePinsHighImpedance(uint64_t mask) {
  gpio_config_t config = {};
  config.pin_bit_mask = mask;
  config.mode = GPIO_MODE_INPUT;
  config.pull_up_en = GPIO_PULLUP_DISABLE;
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  config.intr_type = GPIO_INTR_DISABLE;
  return gpio_config(&config);
}

esp_err_t makePinHighImpedance(gpio_num_t gpio) {
  return makePinsHighImpedance(1ULL << gpio);
}

ReleaseReport releaseAllAndRead() {
  ReleaseReport report = {};
  report.combinedResult = makePinsHighImpedance(kAllSignalMask);
  report.fallbackUsed = report.combinedResult != ESP_OK;
  for (size_t i = 0; i < 5; ++i) {
    report.fallbackResults[i] = ESP_OK;
  }

  if (report.fallbackUsed) {
    for (size_t i = 0; i < 5; ++i) {
      report.fallbackResults[i] = makePinHighImpedance(kSignalGpios[i]);
    }
  }

  report.configurationSucceeded = report.combinedResult == ESP_OK;
  if (!report.configurationSucceeded) {
    report.configurationSucceeded = true;
    for (size_t i = 0; i < 5; ++i) {
      report.configurationSucceeded =
          report.configurationSucceeded && report.fallbackResults[i] == ESP_OK;
    }
  }

  delay(20);
  report.levels = readLevels();
  report.levelsExpected = levelsMatchFixedWiring(report.levels);
  report.verified = report.configurationSucceeded && report.levelsExpected;
  return report;
}

void writePersistentState(PersistentState state) {
  gGuardCookie.magic = 0;
  gGuardCookie.state = static_cast<uint32_t>(state);
  gGuardCookie.invertedState = ~gGuardCookie.state;
  __sync_synchronize();
  gGuardCookie.magic = kGuardMagic;
  __sync_synchronize();
  persistentState = state;
}

void enterFault() {
  faultLatched = true;
  writePersistentState(PersistentState::kFault);
}

bool loadGuardCookie(PersistentState &state) {
  const uint32_t magic = gGuardCookie.magic;
  const uint32_t rawState = gGuardCookie.state;
  const uint32_t inverted = gGuardCookie.invertedState;
  if (magic != kGuardMagic || inverted != ~rawState ||
      rawState > static_cast<uint32_t>(PersistentState::kFault)) {
    return false;
  }
  state = static_cast<PersistentState>(rawState);
  return true;
}

void initializePersistentGuard() {
  PersistentState loaded = PersistentState::kFault;
  const bool cookieValid = loadGuardCookie(loaded);

  // Honor an intact cookie even when the ESP itself reports POWERON. This
  // prevents an ESP-only power cycle from automatically reopening a previous
  // test while the separately powered OLED might never have lost power.
  if (cookieValid) {
    if (loaded == PersistentState::kNeedReset ||
        loaded == PersistentState::kDone ||
        loaded == PersistentState::kFault) {
      persistentState = loaded;
      faultLatched = loaded == PersistentState::kFault;
      return;
    }

    // Any reboot during an action, or after RESET but before ACK, invalidates
    // the controlled sequence and is permanently fail-closed for this cycle.
    enterFault();
    return;
  }

  if (esp_reset_reason() == ESP_RST_POWERON) {
    writePersistentState(PersistentState::kNeedReset);
    return;
  }
  bootCookieInvalid = true;
  enterFault();
}

void recordFailure(const char *&failureStage, const char *stage) {
  if (failureStage == nullptr) {
    failureStage = stage;
  }
}

template <gpio_num_t Pin>
bool pullLow(const char *&failureStage, const char *stage) {
  static_assert(Pin == kSdaIn || Pin == kScl || Pin == kReset,
                "Only SDAIN, SCL and RESET may ever be driven");

  if (gpio_set_level(Pin, 0) != ESP_OK) {
    recordFailure(failureStage, stage);
    return false;
  }

  gpio_config_t config = {};
  config.pin_bit_mask = 1ULL << Pin;
  config.mode = GPIO_MODE_INPUT_OUTPUT_OD;
  config.pull_up_en = GPIO_PULLUP_DISABLE;
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  config.intr_type = GPIO_INTR_DISABLE;
  if (gpio_config(&config) != ESP_OK || gpio_set_level(Pin, 0) != ESP_OK) {
    recordFailure(failureStage, stage);
    return false;
  }

  delayMicroseconds(50);
  if (gpio_get_level(Pin) != 0) {
    recordFailure(failureStage, stage);
    return false;
  }
  return true;
}

template <gpio_num_t Pin>
bool releaseWithoutLevelCheck(const char *&failureStage, const char *stage) {
  static_assert(Pin == kSdaIn || Pin == kScl || Pin == kReset,
                "Only SDAIN, SCL and RESET use this release helper");
  if (makePinHighImpedance(Pin) != ESP_OK) {
    recordFailure(failureStage, stage);
    return false;
  }
  return true;
}

template <gpio_num_t Pin>
bool releaseWaitHigh(const char *&failureStage, const char *stage) {
  if (!releaseWithoutLevelCheck<Pin>(failureStage, stage)) {
    return false;
  }

  const uint32_t deadlineUs = micros() + kLineRiseTimeoutUs;
  while (gpio_get_level(Pin) == 0) {
    if (static_cast<int32_t>(micros() - deadlineUs) >= 0) {
      recordFailure(failureStage, stage);
      return false;
    }
    delayMicroseconds(20);
  }

  delayMicroseconds(kLineHighSettleUs);
  if (gpio_get_level(Pin) == 0) {
    recordFailure(failureStage, stage);
    return false;
  }
  return true;
}

bool transactionDeadlineOkay(uint32_t deadlineUs,
                              const char *&failureStage) {
  if (static_cast<int32_t>(micros() - deadlineUs) < 0) {
    return true;
  }
  recordFailure(failureStage, "transaction absolute deadline");
  return false;
}

bool setAddressBit(bool high, AckReport &report) {
  if (high) {
    if (!releaseWaitHigh<kSdaIn>(report.failureStage,
                                  "address SDA release/rise")) {
      return false;
    }
  } else if (!pullLow<kSdaIn>(report.failureStage,
                               "address SDA LOW/readback")) {
    return false;
  }
  delayMicroseconds(kDataSetupUs);
  return true;
}

bool clockAddressBit(bool expectedHigh, AckReport &report) {
  if (!releaseWaitHigh<kScl>(report.failureStage,
                             "address SCL release/rise")) {
    return false;
  }
  if ((gpio_get_level(kSdaIn) != 0) != expectedHigh) {
    recordFailure(report.failureStage, "address SDA HIGH-phase readback");
    return false;
  }
  delayMicroseconds(kClockHighHoldUs);
  if (gpio_get_level(kScl) == 0) {
    recordFailure(report.failureStage, "address SCL HIGH hold");
    return false;
  }
  if ((gpio_get_level(kSdaIn) != 0) != expectedHigh) {
    recordFailure(report.failureStage, "address SDA HIGH-phase hold");
    return false;
  }
  if (!pullLow<kScl>(report.failureStage,
                     "address SCL LOW/readback")) {
    return false;
  }
  delayMicroseconds(kClockLowHoldUs);
  ++report.bitsCompleted;
  return true;
}

bool performBoundedStop(const char *&failureStage) {
  bool ok = true;
  if (!pullLow<kScl>(failureStage, "STOP SCL LOW")) {
    ok = false;
  }
  delayMicroseconds(kClockLowHoldUs);
  if (!pullLow<kSdaIn>(failureStage, "STOP SDA LOW")) {
    ok = false;
  }
  delayMicroseconds(kDataSetupUs);
  if (!releaseWaitHigh<kScl>(failureStage, "STOP SCL release/rise")) {
    ok = false;
  } else {
    delayMicroseconds(kClockHighHoldUs);
    if (gpio_get_level(kScl) == 0) {
      recordFailure(failureStage, "STOP SCL HIGH hold");
      ok = false;
    }
  }
  // A verified STOP requires a real LOW-to-HIGH SDA edge while SCL is HIGH.
  // Recheck the LOW immediately before releasing SDA; merely observing HIGH
  // after release would not prove that the edge occurred.
  if (gpio_get_level(kSdaIn) != 0) {
    recordFailure(failureStage, "STOP SDA LOW hold");
    ok = false;
  }
  if (!releaseWaitHigh<kSdaIn>(failureStage, "STOP SDA release/rise")) {
    ok = false;
  }
  return ok && gpio_get_level(kScl) != 0 && gpio_get_level(kSdaIn) != 0;
}

ResetReport runExplicitReset() {
  ResetReport report = {};
  report.assertedLevel = -1;
  report.releasedLevel = -1;

  const ReleaseReport initialRelease = releaseAllAndRead();
  if (!initialRelease.configurationSucceeded) {
    recordFailure(report.failureStage, "RESET initial high-impedance config");
  }
  report.initial = readStableLevels();
  if (report.failureStage == nullptr &&
      (!report.initial.stable ||
       !levelsMatchFixedWiring(report.initial.levels))) {
    recordFailure(report.failureStage, "RESET fixed-wiring level gate");
  }

  if (report.failureStage == nullptr &&
      pullLow<kReset>(report.failureStage, "P12 RESET assert")) {
    delay(kResetLowMs);
    report.assertedLevel = gpio_get_level(kReset);
    if (report.assertedLevel != 0) {
      recordFailure(report.failureStage, "P12 RESET LOW hold/readback");
    }
  }

  if (report.failureStage == nullptr &&
      releaseWaitHigh<kReset>(report.failureStage, "P12 RESET release/rise")) {
    report.releasedLevel = gpio_get_level(kReset);
    if (report.releasedLevel == 0) {
      recordFailure(report.failureStage,
                    "P12 RESET released-level verification");
    }
    delay(kResetRecoveryMs);
  }

  report.release = releaseAllAndRead();
  if (!report.release.verified) {
    recordFailure(report.failureStage, "RESET final release verification");
  }
  return report;
}

AckReport runOneAckAttempt() {
  AckReport report = {};
  report.ackP8Sample1 = -1;
  report.ackP8Sample2 = -1;
  report.ackP9Sample1 = -1;
  report.ackP9Sample2 = -1;

  const ReleaseReport initialRelease = releaseAllAndRead();
  if (!initialRelease.configurationSucceeded) {
    recordFailure(report.failureStage, "ACK initial high-impedance config");
  }
  report.initial = readStableLevels();
  if (report.failureStage == nullptr &&
      (!report.initial.stable ||
       !levelsMatchFixedWiring(report.initial.levels))) {
    recordFailure(report.failureStage, "ACK fixed-wiring level gate");
  }

  const uint32_t deadlineUs = micros() + kTransactionLimitUs;
  do {
    if (report.failureStage != nullptr) {
      break;
    }

    // START may occur as soon as this output transition is attempted, so the
    // cleanup path is enabled before the call rather than after it.
    report.startMayHaveOccurred = true;
    if (!pullLow<kSdaIn>(report.failureStage, "START SDA LOW")) {
      break;
    }
    delayMicroseconds(kDataSetupUs);
    if (gpio_get_level(kSdaIn) != 0) {
      recordFailure(report.failureStage, "START SDA LOW hold");
      break;
    }
    if (gpio_get_level(kScl) == 0) {
      recordFailure(report.failureStage, "START SCL HIGH readback");
      break;
    }
    report.startIssued = true;
    if (!pullLow<kScl>(report.failureStage, "START SCL LOW")) {
      break;
    }
    delayMicroseconds(kClockLowHoldUs);

    // Exactly one fixed write-address byte, MSB first.
    for (int bit = 7; bit >= 0; --bit) {
      const bool high = (kAddressWriteByte & (1U << bit)) != 0;
      if (!setAddressBit(high, report) || !clockAddressBit(high, report) ||
          !transactionDeadlineOkay(deadlineUs, report.failureStage)) {
        break;
      }
    }
    if (report.bitsCompleted != 8) {
      break;
    }

    // Ninth clock. P9 is released without requiring HIGH because a unified
    // SDA implementation may acknowledge on P9 instead of split P8/SDAOUT.
    if (!releaseWithoutLevelCheck<kSdaIn>(report.failureStage,
                                           "ACK SDAIN release")) {
      break;
    }
    delayMicroseconds(kDataSetupUs);
    if (!releaseWaitHigh<kScl>(report.failureStage,
                               "ACK SCL release/rise")) {
      break;
    }
    report.ackP8Sample1 = gpio_get_level(kSdaOut);
    report.ackP9Sample1 = gpio_get_level(kSdaIn);
    delayMicroseconds(kAckSampleGapUs);
    if (gpio_get_level(kScl) == 0) {
      recordFailure(report.failureStage, "ACK SCL HIGH between samples");
      break;
    }
    report.ackP8Sample2 = gpio_get_level(kSdaOut);
    report.ackP9Sample2 = gpio_get_level(kSdaIn);
    delayMicroseconds(kClockHighHoldUs);
    if (gpio_get_level(kScl) == 0) {
      recordFailure(report.failureStage, "ACK SCL HIGH hold");
      break;
    }
    if (!pullLow<kScl>(report.failureStage, "ACK SCL LOW")) {
      break;
    }
    delayMicroseconds(kClockLowHoldUs);
    if (!transactionDeadlineOkay(deadlineUs, report.failureStage)) {
      break;
    }
  } while (false);

  if (report.startMayHaveOccurred) {
    report.stopAttempted = true;
    report.stopCompleted = performBoundedStop(report.stopFailureStage);
    if (!report.stopCompleted && report.failureStage == nullptr) {
      recordFailure(report.failureStage, "normal STOP verification");
    }
    if (static_cast<int32_t>(micros() - deadlineUs) >= 0 &&
        report.failureStage == nullptr) {
      recordFailure(report.failureStage,
                    "START-to-STOP absolute deadline");
    }
  }

  // Unconditional final cleanup happens before the first result is printed.
  report.release = releaseAllAndRead();
  if (!report.release.verified && report.failureStage == nullptr) {
    recordFailure(report.failureStage, "ACK final release verification");
  }
  return report;
}

const char *stateName(PersistentState state) {
  switch (state) {
    case PersistentState::kNeedReset:
      return "RESET_REQUIRED";
    case PersistentState::kResetInProgress:
      return "RESET_IN_PROGRESS";
    case PersistentState::kResetQualified:
      return "ACK_READY";
    case PersistentState::kAckInProgress:
      return "ACK_IN_PROGRESS";
    case PersistentState::kDone:
      return "DONE";
    case PersistentState::kFault:
      return "FAULT";
  }
  return "INVALID";
}

void printLevels(const Levels &levels) {
  Serial.printf("  P8 / GPIO0 / SDAOUT = %s\n", levels.p8 ? "HIGH" : "LOW");
  Serial.printf("  P9 / GPIO1 / SDAIN  = %s\n", levels.p9 ? "HIGH" : "LOW");
  Serial.printf("  P10 / GPIO3 / SCL   = %s\n", levels.p10 ? "HIGH" : "LOW");
  Serial.printf("  P11 / GPIO4 / SA0   = %s\n", levels.p11 ? "HIGH" : "LOW");
  Serial.printf("  P12 / GPIO5 / RES#  = %s\n", levels.p12 ? "HIGH" : "LOW");
}

void printReleaseReport(const ReleaseReport &report) {
  Serial.printf(
      "FINAL RELEASE: combined=%d fallbackUsed=%s configVerified=%s ",
      static_cast<int>(report.combinedResult),
      report.fallbackUsed ? "YES" : "NO",
      report.configurationSucceeded ? "YES" : "NO");
  if (report.fallbackUsed) {
    Serial.printf("fallback=[%d,%d,%d,%d,%d] ",
                  static_cast<int>(report.fallbackResults[0]),
                  static_cast<int>(report.fallbackResults[1]),
                  static_cast<int>(report.fallbackResults[2]),
                  static_cast<int>(report.fallbackResults[3]),
                  static_cast<int>(report.fallbackResults[4]));
  }
  Serial.printf("levelsExpected=%s verified=%s\n",
                report.levelsExpected ? "YES" : "NO",
                report.verified ? "YES" : "NO");
  printLevels(report.levels);
}

bool disarmAndRelease() {
  armedAction = ArmedAction::kNone;
  armStartedMs = 0;
  armDeadlineMs = 0;
  const ReleaseReport release = releaseAllAndRead();
  if (!release.configurationSucceeded) {
    enterFault();
    return false;
  }
  return true;
}

bool armExpired() {
  return armedAction != ArmedAction::kNone &&
         static_cast<int32_t>(millis() - armDeadlineMs) >= 0;
}

void discardQueuedSerialInput() {
  delay(20);
  while (Serial.available() != 0) {
    Serial.read();
  }
}

void printStatus() {
  const bool released = disarmAndRelease();
  Serial.println("STATUS: EXPLICIT RESET + ONE-SHOT FIXED 0x3C ACK TEST");
  Serial.printf("  GPIO input configuration: %s\n",
                released ? "VERIFIED" : "FAILED");
  Serial.printf("  Persistent state: %s\n", stateName(persistentState));
  Serial.printf("  Fault latched: %s\n", faultLatched ? "YES" : "NO");
  Serial.printf("  Boot cookie invalid: %s\n",
                bootCookieInvalid ? "YES" : "NO");
  Serial.println("  RESET: one explicit 5-second P12 open-drain LOW");
  Serial.println("  ACK traffic: START -> 0x78 -> one ACK clock -> STOP");
  Serial.println("  Address scan/retry/control/data bytes: unavailable");
  Serial.println("  GPIO HIGH drive and internal pulls: unavailable");
  Serial.println("  P8/SDAOUT and P11/SA0: input only");
  Serial.println("  P9/SDAIN and P10/SCL: LOW or high impedance only");
  Serial.println("  P12/RES#: LOW only during explicit RESET; otherwise input");
}

void printHelp() {
  Serial.println();
  Serial.println("ESP32-C3 OLED explicit-reset, one-shot address ACK test");
  Serial.println("Commands:");
  Serial.println("  STATUS");
  Serial.println("  PINS");
  Serial.println("  ARM RESET");
  Serial.println("  CONFIRM RESET");
  Serial.println("  ARM ACK 0X3C");
  Serial.println("  CONFIRM ACK 0X3C");
  Serial.println("  CANCEL");
  Serial.println("CONFIRM is accepted 1-15 seconds after its matching ARM.");
  Serial.println("RESET must complete before ACK can be armed.");
  Serial.println("Keep P6 at 3.01V through 2.2k; keep P7/P1/P14/P15 floating.");
  printStatus();
}

void printCurrentLevels() {
  const bool released = disarmAndRelease();
  delay(20);
  const StableLevels levels = readStableLevels();
  Serial.printf(
      "Input levels (GPIO input config=%s; required H,H,H,L,H; stable=%s):\n",
      released ? "VERIFIED" : "FAILED", levels.stable ? "YES" : "NO");
  printLevels(levels.levels);
}

void arm(ArmedAction requested) {
  if (!disarmAndRelease()) {
    Serial.println("Rejected: GPIO input configuration failed; terminal fault.");
    return;
  }
  if (faultLatched || persistentState == PersistentState::kDone ||
      persistentState == PersistentState::kFault ||
      persistentState == PersistentState::kResetInProgress ||
      persistentState == PersistentState::kAckInProgress) {
    Serial.println("Rejected: completed/fault/in-progress state. Power-cycle both devices.");
    return;
  }
  if (requested == ArmedAction::kReset &&
      persistentState != PersistentState::kNeedReset) {
    Serial.println("Rejected: RESET is not available in the current state.");
    return;
  }
  if (requested == ArmedAction::kAck &&
      persistentState != PersistentState::kResetQualified) {
    Serial.println("Rejected: complete the explicit RESET stage first.");
    return;
  }

  const StableLevels levels = readStableLevels();
  if (!levels.stable || !levelsMatchFixedWiring(levels.levels)) {
    Serial.println("Rejected: fixed-wiring idle levels are not stable H,H,H,L,H.");
    printLevels(levels.levels);
    Serial.println("Power off before inspecting any connection.");
    return;
  }

  armedAction = requested;
  armStartedMs = millis();
  armDeadlineMs = armStartedMs + kArmTimeoutMs;
  if (requested == ArmedAction::kReset) {
    Serial.println("ARMED RESET for 15 seconds.");
    Serial.println("After at least 1 second, send: CONFIRM RESET");
  } else {
    Serial.println("ARMED ACK 0x3C for 15 seconds.");
    Serial.println("After at least 1 second, send: CONFIRM ACK 0X3C");
  }
  discardQueuedSerialInput();
}

bool confirmationTimingValid(ArmedAction expected) {
  if (armedAction != expected) {
    const bool released = disarmAndRelease();
    Serial.println("Rejected: CONFIRM does not match the armed action.");
    if (!released) {
      Serial.println("RELEASE UNVERIFIED: terminal fault.");
    }
    return false;
  }
  if (armExpired()) {
    const bool released = disarmAndRelease();
    Serial.println(released
                       ? "Rejected: ARM expired. All GPIOs are verified as inputs."
                       : "Rejected: ARM expired; release unverified, terminal fault.");
    return false;
  }
  if (static_cast<uint32_t>(millis() - armStartedMs) <
      kMinimumConfirmDelayMs) {
    const bool released = disarmAndRelease();
    Serial.println("Rejected: CONFIRM arrived too soon; ARM again manually.");
    if (!released) {
      Serial.println("RELEASE UNVERIFIED: terminal fault.");
    }
    return false;
  }
  armedAction = ArmedAction::kNone;
  armStartedMs = 0;
  armDeadlineMs = 0;
  return true;
}

void confirmReset() {
  if (!confirmationTimingValid(ArmedAction::kReset)) {
    return;
  }
  if (faultLatched || persistentState != PersistentState::kNeedReset) {
    const bool released = disarmAndRelease();
    Serial.println("Rejected: RESET is unavailable in the current state.");
    if (!released) {
      Serial.println("RELEASE UNVERIFIED: terminal fault.");
    }
    return;
  }

  // Persist the in-progress state before any GPIO action or progress log. A
  // reset before the final state write will be recognized as an interrupted
  // action and converted to FAULT on the next boot.
  writePersistentState(PersistentState::kResetInProgress);
  Serial.println("RUNNING ONCE: P12/RES# LOW for 5 seconds, then release.");
  Serial.println("You may observe P6 now; no serial output occurs while LOW.");
  Serial.flush();
  const ResetReport report = runExplicitReset();

  Serial.printf("RESET: asserted=%s released=%s\n",
                report.assertedLevel == 0 ? "LOW" : "NOT-VERIFIED",
                report.releasedLevel > 0 ? "HIGH" : "NOT-VERIFIED");
  printReleaseReport(report.release);
  if (report.failureStage != nullptr || !report.release.verified) {
    enterFault();
    Serial.printf("RESET FAILED AT: %s\n",
                  report.failureStage != nullptr ? report.failureStage
                                                 : "final verification");
    Serial.println("FAULT LATCHED: power OLED and ESP32 off now.");
    return;
  }

  writePersistentState(PersistentState::kResetQualified);
  Serial.println("RESET QUALIFIED. P12 is high impedance again.");
  Serial.println("Do not power-cycle or change wiring before the ACK stage.");
  Serial.println("Next command: ARM ACK 0X3C");
}

void printAckResult(const AckReport &report) {
  const bool resultValid = report.failureStage == nullptr &&
                           report.startIssued && report.bitsCompleted == 8 &&
                           report.ackP8Sample1 >= 0 &&
                           report.ackP8Sample2 >= 0 &&
                           report.ackP9Sample1 >= 0 &&
                           report.ackP9Sample2 >= 0 &&
                           report.stopCompleted && report.release.verified &&
                           report.initial.stable &&
                           levelsMatchFixedWiring(report.initial.levels);
  if (!resultValid) {
    Serial.println("ACK RESULT: INVALID (sequence/STOP/release not verified)");
    return;
  }

  const bool p8StableLow =
      report.ackP8Sample1 == 0 && report.ackP8Sample2 == 0;
  const bool p8StableHigh =
      report.ackP8Sample1 > 0 && report.ackP8Sample2 > 0;
  if (p8StableLow) {
    Serial.println(
        "ACK RESULT: ACK ON P8/SDAOUT (fixed split-SDA candidate supported)");
    if (report.ackP9Sample1 == 0 && report.ackP9Sample2 == 0) {
      Serial.println(
          "P9 DIAGNOSTIC: also LOW; D1/D2 coupling or alternate topology remains possible.");
    }
  } else if (p8StableHigh) {
    Serial.println("ACK RESULT: NO ACK ON P8/SDAOUT");
    if (report.ackP9Sample1 == 0 && report.ackP9Sample2 == 0) {
      Serial.println(
          "P9 DIAGNOSTIC: LOW on both samples, but it is not accepted as this fixed mapping's ACK.");
    }
  } else {
    Serial.println("ACK RESULT: INDETERMINATE (P8 samples were not stable)");
  }
}

void confirmAck() {
  if (!confirmationTimingValid(ArmedAction::kAck)) {
    return;
  }
  if (faultLatched || persistentState != PersistentState::kResetQualified) {
    const bool released = disarmAndRelease();
    Serial.println("Rejected: ACK is unavailable in the current state.");
    if (!released) {
      Serial.println("RELEASE UNVERIFIED: terminal fault.");
    }
    return;
  }

  // Persist the in-progress state before any bus edge or progress log. There
  // is no serial command that can reopen an interrupted or completed action.
  writePersistentState(PersistentState::kAckInProgress);
  Serial.println("RUNNING ONCE: START -> 0x78 -> one ACK clock -> STOP.");
  Serial.flush();
  const AckReport report = runOneAckAttempt();

  const char *startStatus = report.startIssued
                                ? "VERIFIED"
                                : report.startMayHaveOccurred ? "MAYBE" : "NO";
  Serial.printf("ADDRESS: 0x%02X, bits completed=%u/8, START=%s STOP=%s\n",
                kAddressWriteByte, report.bitsCompleted, startStatus,
                report.stopCompleted ? "VERIFIED" : "UNVERIFIED");
  Serial.printf(
      "ACK SAMPLES: P8=%s/%s P9=%s/%s\n",
      report.ackP8Sample1 == 0 ? "LOW" :
      report.ackP8Sample1 > 0 ? "HIGH" : "N/A",
      report.ackP8Sample2 == 0 ? "LOW" :
      report.ackP8Sample2 > 0 ? "HIGH" : "N/A",
      report.ackP9Sample1 == 0 ? "LOW" :
      report.ackP9Sample1 > 0 ? "HIGH" : "N/A",
      report.ackP9Sample2 == 0 ? "LOW" :
      report.ackP9Sample2 > 0 ? "HIGH" : "N/A");
  if (report.failureStage != nullptr) {
    Serial.printf("SEQUENCE FAILURE: %s\n", report.failureStage);
  }
  if (report.stopFailureStage != nullptr) {
    Serial.printf("STOP DETAIL: %s\n", report.stopFailureStage);
  }
  printReleaseReport(report.release);
  printAckResult(report);

  if (report.failureStage != nullptr || !report.stopCompleted ||
      !report.release.verified) {
    enterFault();
    Serial.println("FAULT/INVALID TERMINAL STATE: power OLED and ESP32 off.");
  } else {
    writePersistentState(PersistentState::kDone);
    Serial.println("ALL ESP32 OUTPUT PATHS RELEASED AND VERIFIED.");
    Serial.println("Test is terminal; power-cycle OLED and ESP32 before more work.");
  }
}

}  // namespace

void setup() {
  const ReleaseReport bootRelease = releaseAllAndRead();
  initializePersistentGuard();
  if (!bootRelease.configurationSucceeded) {
    enterFault();
  }

  Serial.begin(115200);
  Serial.setTimeout(100);
  delay(1000);
  printHelp();
  if (!bootRelease.configurationSucceeded) {
    Serial.println("BOOT FAULT: GPIO input configuration was not verified.");
  }
}

void loop() {
  if (armExpired()) {
    const bool released = disarmAndRelease();
    Serial.println(released
                       ? "ARM expired. All GPIOs are verified as inputs."
                       : "ARM expired, but GPIO input configuration failed; terminal fault.");
  }

  if (Serial.available() == 0) {
    delay(10);
    return;
  }

  String command = Serial.readStringUntil('\n');
  command.trim();
  command.toUpperCase();

  if (command == "STATUS") {
    printStatus();
  } else if (command == "PINS") {
    printCurrentLevels();
  } else if (command == "HELP") {
    printHelp();
  } else if (command == "CANCEL") {
    const bool released = disarmAndRelease();
    Serial.println(released
                       ? "Cancelled. All GPIOs are verified as inputs."
                       : "Cancel release failed; terminal fault.");
  } else if (command == "ARM RESET") {
    arm(ArmedAction::kReset);
  } else if (command == "CONFIRM RESET") {
    confirmReset();
  } else if (command == "ARM ACK 0X3C") {
    arm(ArmedAction::kAck);
  } else if (command == "CONFIRM ACK 0X3C") {
    confirmAck();
  } else if (command.length() != 0) {
    const bool released = disarmAndRelease();
    Serial.println("Unknown or rejected command. Use HELP.");
    if (!released) {
      Serial.println("RELEASE UNVERIFIED: terminal fault.");
    }
  }
}
