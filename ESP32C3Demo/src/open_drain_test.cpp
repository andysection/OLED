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
uint32_t armDeadlineMs = 0;

esp_err_t makeHighImpedance(const SignalPin &signal) {
  gpio_config_t config = {};
  config.pin_bit_mask = 1ULL << signal.gpio;
  config.mode = GPIO_MODE_INPUT;
  config.pull_up_en = GPIO_PULLUP_DISABLE;
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  config.intr_type = GPIO_INTR_DISABLE;
  return gpio_config(&config);
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
  armDeadlineMs = 0;
  releaseAllSignals();
}

bool armExpired() {
  return armedSignal != nullptr &&
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
  Serial.println("STATUS: SINGLE-PIN OPEN-DRAIN TEST");
  Serial.println("  Default state: all five GPIOs floating inputs");
  Serial.println("  Action: one selected GPIO pulls LOW for 5 seconds");
  Serial.println("  Automatic release: enabled");
  Serial.println("  High-level output, scanning and repeated pulses: unavailable");
}

void printHelp() {
  Serial.println();
  Serial.println("ESP32-C3 OLED single-pin open-drain test");
  Serial.println("Commands:");
  Serial.println("  STATUS");
  Serial.println("  PINS");
  Serial.println("  ARM P8|P9|P10|P11|P12");
  Serial.println("  CONFIRM P8|P9|P10|P11|P12");
  Serial.println("  CANCEL");
  Serial.println("CONFIRM must match the armed pin within 15 seconds.");
  Serial.println("Place the voltmeter on OLED P6-to-P13 before CONFIRM.");
  printStatus();
}

void armSignal(const String &name) {
  disarm();
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

void confirmSignal(const String &name) {
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

  Serial.printf("RELEASED: OLED P%u / GPIO%u is high impedance again.\n",
                signal.oledPad, static_cast<unsigned>(signal.gpio));
  Serial.printf("GPIO%u release result: config=%d readback=%s\n",
                static_cast<unsigned>(signal.gpio),
                static_cast<int>(releaseResult),
                releasedLevel ? "HIGH" : "LOW");
  Serial.println("Power the OLED off before changing any wiring.");
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
  } else if (command.startsWith("ARM ")) {
    armSignal(command.substring(4));
  } else if (command.startsWith("CONFIRM ")) {
    confirmSignal(command.substring(8));
  } else if (command.length() != 0) {
    disarm();
    Serial.println("Unknown command. Use HELP.");
  }
}
