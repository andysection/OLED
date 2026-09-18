#include <Arduino.h>
#include <driver/gpio.h>

#ifndef OPEN_DRAIN_TEST_BUILD
#define OPEN_DRAIN_TEST_BUILD 0
#endif

static_assert(OPEN_DRAIN_TEST_BUILD == 1,
              "Select the open_drain_test environment explicitly");

namespace {

constexpr uint32_t kArmTimeoutMs = 15000;
constexpr uint32_t kLowPulseMs = 5000;

struct SignalPin {
  uint8_t oledPad;
  gpio_num_t gpio;
};

constexpr SignalPin kSignalPins[] = {
    {8, GPIO_NUM_0},
    {9, GPIO_NUM_1},
    {10, GPIO_NUM_3},
    {11, GPIO_NUM_4},
    {12, GPIO_NUM_5},
};

const SignalPin *armedSignal = nullptr;
bool pairArmed = false;
bool releaseFaultLatched = false;
uint32_t armDeadlineMs = 0;

esp_err_t makePinsHighImpedance(uint64_t pinMask) {
  gpio_config_t config = {};
  config.pin_bit_mask = pinMask;
  config.mode = GPIO_MODE_INPUT;
  config.pull_up_en = GPIO_PULLUP_DISABLE;
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  config.intr_type = GPIO_INTR_DISABLE;
  return gpio_config(&config);
}

esp_err_t makeHighImpedance(const SignalPin &signal) {
  return makePinsHighImpedance(1ULL << signal.gpio);
}

void releaseAllSignals() {
  for (const SignalPin &signal : kSignalPins) {
    makeHighImpedance(signal);
  }
}

const SignalPin *findSignal(const String &name) {
  for (const SignalPin &signal : kSignalPins) {
    String expected = "P";
    expected += signal.oledPad;
    if (name == expected) {
      return &signal;
    }
  }
  return nullptr;
}

void disarm() {
  armedSignal = nullptr;
  pairArmed = false;
  armDeadlineMs = 0;
  releaseAllSignals();
}

bool armExpired() {
  return (armedSignal != nullptr || pairArmed) &&
         static_cast<int32_t>(millis() - armDeadlineMs) >= 0;
}

void printLevels() {
  releaseAllSignals();
  delayMicroseconds(100);
  Serial.println("Released input levels:");
  for (const SignalPin &signal : kSignalPins) {
    Serial.printf("  OLED P%u / GPIO%u = %s\n", signal.oledPad,
                  static_cast<unsigned>(signal.gpio),
                  gpio_get_level(signal.gpio) ? "HIGH" : "LOW");
  }
}

void printStatus() {
  releaseAllSignals();
  Serial.println("STATUS: BOUNDED OPEN-DRAIN TEST");
  Serial.println("  Default state: all five GPIOs floating inputs");
  Serial.println("  Action: one selected GPIO pulls LOW for 5 seconds");
  Serial.println("  Pair action: only P9 + P10 may pull LOW together");
  Serial.println("  Automatic release: enabled");
  Serial.println("  High-level output, scanning and automatic repetition: unavailable");
  if (releaseFaultLatched) {
    Serial.println("  FAULT: release was not verified; power-cycle before any new test");
  }
}

void printHelp() {
  Serial.println();
  Serial.println("ESP32-C3 OLED bounded open-drain test");
  Serial.println("Commands:");
  Serial.println("  STATUS");
  Serial.println("  PINS");
  Serial.println("  ARM P8|P9|P10|P11|P12");
  Serial.println("  CONFIRM P8|P9|P10|P11|P12");
  Serial.println("  ARM P9+P10");
  Serial.println("  CONFIRM P9+P10");
  Serial.println("  CANCEL");
  Serial.println("CONFIRM must match the armed pin within 15 seconds.");
  Serial.println("P9+P10 is the only permitted two-pin combination.");
  Serial.println("Place the voltmeter on OLED P6-to-P13 before CONFIRM.");
  printStatus();
}

void armSignal(const String &name) {
  disarm();
  if (releaseFaultLatched) {
    Serial.println("Rejected: release fault is latched. Power-cycle the ESP32.");
    return;
  }
  const SignalPin *signal = findSignal(name);
  if (signal == nullptr) {
    Serial.println("Rejected: choose P8, P9, P10, P11 or P12.");
    return;
  }

  armedSignal = signal;
  armDeadlineMs = millis() + kArmTimeoutMs;
  Serial.printf("ARMED: OLED P%u / GPIO%u for 15 seconds.\n",
                signal->oledPad, static_cast<unsigned>(signal->gpio));
  Serial.printf("To run one 5-second LOW pulse, send: CONFIRM P%u\n",
                signal->oledPad);
}

void armPair() {
  disarm();
  if (releaseFaultLatched) {
    Serial.println("Rejected: release fault is latched. Power-cycle the ESP32.");
    return;
  }
  pairArmed = true;
  armDeadlineMs = millis() + kArmTimeoutMs;
  Serial.println("ARMED PAIR: OLED P9 / GPIO1 + OLED P10 / GPIO3 for 15 seconds.");
  Serial.println("To run one 5-second paired LOW pulse, send: CONFIRM P9+P10");
}

void confirmSignal(const String &name) {
  if (releaseFaultLatched) {
    disarm();
    Serial.println("Rejected: release fault is latched. Power-cycle the ESP32.");
    return;
  }
  if (armExpired()) {
    disarm();
    Serial.println("Rejected: ARM expired. All GPIOs are high impedance.");
    return;
  }

  const SignalPin *requested = findSignal(name);
  if (armedSignal == nullptr || requested == nullptr ||
      requested != armedSignal) {
    disarm();
    Serial.println("Rejected: CONFIRM does not match the armed pin.");
    return;
  }

  const SignalPin signal = *armedSignal;
  armedSignal = nullptr;
  armDeadlineMs = 0;

  Serial.printf("PULLING LOW: OLED P%u / GPIO%u for 5 seconds.\n",
                signal.oledPad, static_cast<unsigned>(signal.gpio));
  Serial.println("Measure OLED P6-to-P13 now.");

  // Preload LOW before enabling the output path. INPUT_OUTPUT_OD keeps the
  // input buffer enabled so the pad level can be independently read back.
  const esp_err_t preloadResult = gpio_set_level(signal.gpio, 0);
  gpio_config_t lowConfig = {};
  lowConfig.pin_bit_mask = 1ULL << signal.gpio;
  lowConfig.mode = GPIO_MODE_INPUT_OUTPUT_OD;
  lowConfig.pull_up_en = GPIO_PULLUP_DISABLE;
  lowConfig.pull_down_en = GPIO_PULLDOWN_DISABLE;
  lowConfig.intr_type = GPIO_INTR_DISABLE;
  const esp_err_t configResult = gpio_config(&lowConfig);
  const esp_err_t driveResult = gpio_set_level(signal.gpio, 0);

  delay(20);
  const int assertedLevel = gpio_get_level(signal.gpio);
  Serial.printf(
      "GPIO%u assert results: preload=%d config=%d drive=%d readback=%s\n",
      static_cast<unsigned>(signal.gpio), static_cast<int>(preloadResult),
      static_cast<int>(configResult), static_cast<int>(driveResult),
      assertedLevel ? "HIGH" : "LOW");

  delay(kLowPulseMs - 20);
  const esp_err_t releaseResult = makeHighImpedance(signal);
  delay(20);
  const int releasedLevel = gpio_get_level(signal.gpio);

  Serial.printf("GPIO%u release result: config=%d readback=%s\n",
                static_cast<unsigned>(signal.gpio),
                static_cast<int>(releaseResult),
                releasedLevel ? "HIGH" : "LOW");
  if (releaseResult == ESP_OK && releasedLevel != 0) {
    Serial.printf("RELEASED VERIFIED: OLED P%u / GPIO%u is high impedance again.\n",
                  signal.oledPad, static_cast<unsigned>(signal.gpio));
    Serial.println("Power the OLED off before changing any wiring.");
  } else {
    releaseFaultLatched = true;
    Serial.println("RELEASE UNVERIFIED: power the OLED off now.");
  }
}

struct PairReleaseStatus {
  esp_err_t combinedResult;
  esp_err_t fallbackP9Result;
  esp_err_t fallbackP10Result;
  int readbackP9;
  int readbackP10;
  bool fallbackUsed;
  bool verified;
};

PairReleaseStatus releasePairAndVerify() {
  constexpr gpio_num_t kP9Gpio = kSignalPins[1].gpio;
  constexpr gpio_num_t kP10Gpio = kSignalPins[2].gpio;
  constexpr uint64_t kP9Mask = 1ULL << kP9Gpio;
  constexpr uint64_t kP10Mask = 1ULL << kP10Gpio;
  constexpr uint64_t kPairMask = kP9Mask | kP10Mask;

  PairReleaseStatus status = {
      makePinsHighImpedance(kPairMask), ESP_OK, ESP_OK, 0, 0, false, false};

  // If the combined release call fails, retry each GPIO before doing any
  // serial output so neither output is intentionally held LOW while logging.
  if (status.combinedResult != ESP_OK) {
    status.fallbackUsed = true;
    status.fallbackP9Result = makePinsHighImpedance(kP9Mask);
    status.fallbackP10Result = makePinsHighImpedance(kP10Mask);
  }

  delay(20);
  status.readbackP9 = gpio_get_level(kP9Gpio);
  status.readbackP10 = gpio_get_level(kP10Gpio);

  // A LOW readback is unexpected with the validated external pull-ups. Retry
  // each pin separately, then report the final state without claiming success
  // unless both lines read HIGH.
  if ((status.readbackP9 == 0 || status.readbackP10 == 0) &&
      !status.fallbackUsed) {
    status.fallbackUsed = true;
    status.fallbackP9Result = makePinsHighImpedance(kP9Mask);
    status.fallbackP10Result = makePinsHighImpedance(kP10Mask);
    delay(20);
    status.readbackP9 = gpio_get_level(kP9Gpio);
    status.readbackP10 = gpio_get_level(kP10Gpio);
  }

  const bool releaseCallSucceeded =
      status.combinedResult == ESP_OK ||
      (status.fallbackP9Result == ESP_OK &&
       status.fallbackP10Result == ESP_OK);
  status.verified = releaseCallSucceeded && status.readbackP9 != 0 &&
                    status.readbackP10 != 0;
  return status;
}

void printPairReleaseStatus(const PairReleaseStatus &status) {
  Serial.printf(
      "PAIR release results: combined=%d fallbackUsed=%s fallbackP9=%d "
      "fallbackP10=%d readbackP9=%s readbackP10=%s\n",
      static_cast<int>(status.combinedResult),
      status.fallbackUsed ? "YES" : "NO",
      static_cast<int>(status.fallbackP9Result),
      static_cast<int>(status.fallbackP10Result),
      status.readbackP9 ? "HIGH" : "LOW",
      status.readbackP10 ? "HIGH" : "LOW");
}

void confirmPair() {
  if (releaseFaultLatched) {
    disarm();
    Serial.println("Rejected: release fault is latched. Power-cycle the ESP32.");
    return;
  }
  if (armExpired()) {
    disarm();
    Serial.println("Rejected: ARM expired. All GPIOs are high impedance.");
    return;
  }

  if (!pairArmed || armedSignal != nullptr) {
    disarm();
    Serial.println("Rejected: CONFIRM P9+P10 does not match the armed action.");
    return;
  }

  pairArmed = false;
  armDeadlineMs = 0;

  constexpr gpio_num_t kP9Gpio = kSignalPins[1].gpio;
  constexpr gpio_num_t kP10Gpio = kSignalPins[2].gpio;
  constexpr uint64_t kPairMask =
      (1ULL << kP9Gpio) | (1ULL << kP10Gpio);

  Serial.println(
      "PULLING LOW PAIR: OLED P9 / GPIO1 + OLED P10 / GPIO3 for 5 seconds.");
  Serial.println("Measure OLED P6-to-P13 now.");

  const esp_err_t preloadP9 = gpio_set_level(kP9Gpio, 0);
  const esp_err_t preloadP10 = gpio_set_level(kP10Gpio, 0);
  if (preloadP9 != ESP_OK || preloadP10 != ESP_OK) {
    const PairReleaseStatus releaseStatus = releasePairAndVerify();
    releaseFaultLatched = true;
    Serial.printf("PAIR ABORTED BEFORE OUTPUT: preloadP9=%d preloadP10=%d.\n",
                  static_cast<int>(preloadP9),
                  static_cast<int>(preloadP10));
    printPairReleaseStatus(releaseStatus);
    Serial.println("FAULT LATCHED: power the OLED and ESP32 off now.");
    return;
  }

  gpio_config_t lowConfig = {};
  lowConfig.pin_bit_mask = kPairMask;
  lowConfig.mode = GPIO_MODE_INPUT_OUTPUT_OD;
  lowConfig.pull_up_en = GPIO_PULLUP_DISABLE;
  lowConfig.pull_down_en = GPIO_PULLDOWN_DISABLE;
  lowConfig.intr_type = GPIO_INTR_DISABLE;
  const esp_err_t configResult = gpio_config(&lowConfig);
  const uint32_t lowStartedMs = millis();
  if (configResult != ESP_OK) {
    const PairReleaseStatus releaseStatus = releasePairAndVerify();
    releaseFaultLatched = true;
    Serial.printf("PAIR ABORTED: combined output config=%d.\n",
                  static_cast<int>(configResult));
    printPairReleaseStatus(releaseStatus);
    Serial.println("FAULT LATCHED: power the OLED and ESP32 off now.");
    return;
  }

  const esp_err_t driveP9 = gpio_set_level(kP9Gpio, 0);
  const esp_err_t driveP10 = gpio_set_level(kP10Gpio, 0);
  if (driveP9 != ESP_OK || driveP10 != ESP_OK) {
    const PairReleaseStatus releaseStatus = releasePairAndVerify();
    releaseFaultLatched = true;
    Serial.printf("PAIR ABORTED: driveP9=%d driveP10=%d.\n",
                  static_cast<int>(driveP9), static_cast<int>(driveP10));
    printPairReleaseStatus(releaseStatus);
    Serial.println("FAULT LATCHED: power the OLED and ESP32 off now.");
    return;
  }

  delay(20);
  const int assertedP9 = gpio_get_level(kP9Gpio);
  const int assertedP10 = gpio_get_level(kP10Gpio);
  const bool readbackFailed = assertedP9 != 0 || assertedP10 != 0;
  if (readbackFailed) {
    const PairReleaseStatus releaseStatus = releasePairAndVerify();
    releaseFaultLatched = true;
    Serial.printf(
        "PAIR assert results: preloadP9=%d preloadP10=%d config=%d "
        "driveP9=%d driveP10=%d readbackP9=%s readbackP10=%s\n",
        static_cast<int>(preloadP9), static_cast<int>(preloadP10),
        static_cast<int>(configResult), static_cast<int>(driveP9),
        static_cast<int>(driveP10), assertedP9 ? "HIGH" : "LOW",
        assertedP10 ? "HIGH" : "LOW");
    Serial.println(
        "PAIR ABORTED: configuration or LOW readback failed; outputs were released immediately.");
    printPairReleaseStatus(releaseStatus);
    Serial.println("FAULT LATCHED: power the OLED and ESP32 off now.");
    return;
  }

  // Do not write to Serial while the pair is held LOW. Release at the absolute
  // deadline first so logging can never extend the requested pulse duration.
  const int32_t remainingMs = static_cast<int32_t>(
      (lowStartedMs + kLowPulseMs) - millis());
  if (remainingMs > 0) {
    delay(static_cast<uint32_t>(remainingMs));
  }
  const PairReleaseStatus releaseStatus = releasePairAndVerify();
  Serial.printf(
      "PAIR assert results: preloadP9=%d preloadP10=%d config=%d "
      "driveP9=%d driveP10=%d readbackP9=%s readbackP10=%s\n",
      static_cast<int>(preloadP9), static_cast<int>(preloadP10),
      static_cast<int>(configResult), static_cast<int>(driveP9),
      static_cast<int>(driveP10), assertedP9 ? "HIGH" : "LOW",
      assertedP10 ? "HIGH" : "LOW");
  printPairReleaseStatus(releaseStatus);
  if (releaseStatus.verified) {
    Serial.println(
        "RELEASED PAIR VERIFIED: OLED P9 / GPIO1 and OLED P10 / GPIO3 are high impedance again.");
    Serial.println("Power the OLED off before changing any wiring.");
  } else {
    releaseFaultLatched = true;
    Serial.println(
        "PAIR RELEASE UNVERIFIED; FAULT LATCHED: power the OLED and ESP32 off now.");
  }
}

}  // namespace

void setup() {
  releaseAllSignals();
  Serial.begin(115200);
  Serial.setTimeout(100);
  delay(1000);
  printHelp();
}

void loop() {
  if (armExpired()) {
    disarm();
    Serial.println("ARM expired. All GPIOs remain high impedance.");
  }

  if (Serial.available() == 0) {
    delay(10);
    return;
  }

  String command = Serial.readStringUntil('\n');
  command.trim();
  command.toUpperCase();

  if (command == "STATUS") {
    disarm();
    printStatus();
  } else if (command == "PINS") {
    disarm();
    printLevels();
  } else if (command == "HELP") {
    disarm();
    printHelp();
  } else if (command == "CANCEL") {
    disarm();
    Serial.println("Cancelled. All GPIOs are high impedance.");
  } else if (command == "ARM P9+P10") {
    armPair();
  } else if (command == "CONFIRM P9+P10") {
    confirmPair();
  } else if (command.startsWith("ARM ")) {
    armSignal(command.substring(4));
  } else if (command.startsWith("CONFIRM ")) {
    confirmSignal(command.substring(8));
  } else if (command.length() != 0) {
    disarm();
    Serial.println("Unknown command. Use HELP.");
  }
}
