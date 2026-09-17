#include <Arduino.h>
#include <driver/gpio.h>

#ifndef OLED_POWERED_PROBE_ENABLED
#define OLED_POWERED_PROBE_ENABLED 0
#endif

static_assert(
    OLED_POWERED_PROBE_ENABLED == 0,
    "Powered OLED probing is intentionally unavailable in this locked build");

namespace {

struct SignalPin {
  uint8_t oledPad;
  gpio_num_t gpio;
  const char *headerPin;
  const char *candidate;
};

// AirM2M ESP32-C3 board, antenna up and component side facing the user.
// These assignments are candidates only; the OLED functions still require an
// ACK test after a separate power-integrity review.
constexpr SignalPin kSignalPins[] = {
    {8, GPIO_NUM_0, "02", "D2 / SDAOUT"},
    {9, GPIO_NUM_1, "03", "D1 / SDAIN"},
    {10, GPIO_NUM_3, "20", "D0 / SCL"},
    {11, GPIO_NUM_4, "28", "D/C# / SA0"},
    {12, GPIO_NUM_5, "27", "RES#"},
};

void makeHighImpedance(const SignalPin &signal) {
  gpio_set_direction(signal.gpio, GPIO_MODE_INPUT);
  gpio_set_pull_mode(signal.gpio, GPIO_FLOATING);
}

void enforceLockedState() {
  for (const SignalPin &signal : kSignalPins) {
    makeHighImpedance(signal);
  }
}

void printPinMap() {
  Serial.println("Candidate signal map (not functionally confirmed):");
  for (const SignalPin &signal : kSignalPins) {
    Serial.printf("  OLED P%u -> GPIO%u / header %s -> %s\n",
                  signal.oledPad, static_cast<unsigned>(signal.gpio),
                  signal.headerPin, signal.candidate);
  }
}

void printLevels() {
  enforceLockedState();
  delayMicroseconds(100);

  Serial.println("Read-only GPIO levels (floating inputs may vary):");
  for (const SignalPin &signal : kSignalPins) {
    const int level = gpio_get_level(signal.gpio);
    Serial.printf("  OLED P%u / GPIO%u = %s\n", signal.oledPad,
                  static_cast<unsigned>(signal.gpio), level ? "HIGH" : "LOW");
  }
  Serial.println("No internal pull-up or pull-down is enabled.");
}

void printStatus() {
  enforceLockedState();
  Serial.println("STATUS: LOCKED / READ ONLY");
  Serial.println("  GPIO drive operations: unavailable");
  Serial.println("  P6: external VDD candidate; never connect to a GPIO");
  Serial.println("  P7: internal VLH candidate; leave unpowered");
  Serial.println("  P13: VSS / VLL candidate");
  Serial.println("  P1, P14, P15: unresolved analog nodes; leave unpowered");
  Serial.println("  P8-P12 functions remain candidates until an ACK test");
}

void printHelp() {
  Serial.println();
  Serial.println("ESP32-C3 OLED pin investigation - locked firmware");
  Serial.println("Commands: PINS, STATUS, HELP");
  Serial.println("ARM and SCAN are intentionally rejected.");
  printPinMap();
  printStatus();
}

void rejectActiveProbe() {
  enforceLockedState();
  Serial.println("LOCKED: active probing is not present in this firmware.");
  Serial.println("All five GPIOs remain floating inputs.");
}

}  // namespace

void setup() {
  enforceLockedState();
  Serial.begin(115200);
  Serial.setTimeout(100);
  delay(1000);
  printHelp();
}

void loop() {
  // Reassert the safe state even if a library or a reset path touched a pin.
  enforceLockedState();

  if (Serial.available() == 0) {
    delay(10);
    return;
  }

  String command = Serial.readStringUntil('\n');
  command.trim();
  command.toUpperCase();

  if (command == "PINS") {
    printLevels();
  } else if (command == "STATUS") {
    printStatus();
  } else if (command == "HELP") {
    printHelp();
  } else if (command == "SCAN" || command.startsWith("ARM")) {
    rejectActiveProbe();
  } else if (command.length() != 0) {
    Serial.println("Unknown command. Use PINS, STATUS or HELP.");
  }

  enforceLockedState();
}
