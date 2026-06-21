/*
  ============================================================
  8-WIRE STEPPER MOTOR UNIVERSAL TESTER
  ============================================================
  Hardware:
    - Arduino Uno R3
    - 2x ADS1115 16-bit ADC (I2C addr 0x48 and 0x49)
    - 1x SSD1315 0.96" 128x64 OLED (I2C addr 0x3C, SSD1306-compatible)
    - 8x 100 ohm reference resistors (one per wire channel)
    - 8-wire test harness (clip leads / header pins)

  Wiring:
    Wire N -----+---- Arduino D2..D9 (drive/ground control)
                |
               [100 ohm]
                |
                +---- ADS1115 channel (voltage sense node)

    Each wire has its OWN resistor between the Arduino digital pin
    and the actual sense/motor node. This protects the Arduino pin
    if two "drive" pins ever get set HIGH/LOW at once by accident,
    and it doubles as the voltage-divider reference resistor.

    ADS1115 #1 (addr 0x48): A0-A3 = Wires 1-4
    ADS1115 #2 (addr 0x49): A0-A3 = Wires 5-8

    Arduino digital pins:
      D2 = Wire 1   D3 = Wire 2   D4 = Wire 3   D5 = Wire 4
      D6 = Wire 5   D7 = Wire 6   D8 = Wire 7   D9 = Wire 8

    I2C bus (shared):
      A4 (SDA) -> ADS1115 #1 SDA, ADS1115 #2 SDA, OLED SDA
      A5 (SCL) -> ADS1115 #1 SCL, ADS1115 #2 SCL, OLED SCL

  Libraries required (Install via Library Manager):
    - Adafruit_ADS1X15
    - Adafruit_SSD1306
    - Adafruit_GFX

  ============================================================
  HOW THE TEST WORKS
  ============================================================
  For every unique pair of wires (28 pairs total for 8 wires):
    1. Set "drive" wire's Arduino pin to OUTPUT HIGH (5V through its
       100 ohm resistor)
    2. Set "ground" wire's Arduino pin to OUTPUT LOW
    3. Set all other 6 wires to INPUT (high-Z / floating) so they
       don't interfere with the measurement
    4. Read the voltage AT THE DRIVE WIRE'S SENSE NODE via its
       ADS1115 channel
    5. Calculate resistance using the voltage divider formula
    6. Store result in an 8x8 resistance matrix

  After all 28 pairs are measured, the firmware looks at the
  matrix and tries to classify the motor:
    - Groups of 2 wires with similar resistance, isolated from
      all other wires -> bipolar coil pairs
    - A 3rd wire reading ~half the resistance to each end of a
      pair -> that 3rd wire is a center tap (unipolar motor)
    - Open / no-connection pairs are ignored

  Results are printed to Serial AND shown on the OLED.
  ============================================================
*/

#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---------------- CONFIG ----------------
#define NUM_WIRES        8
#define REF_RESISTOR_OHMS 100.0   // value of resistor on each wire channel
#define ADC_VREF         5.0      // Arduino 5V rail used as ADC comparison voltage
#define SETTLE_TIME_MS   5        // time to let voltage settle before reading
#define OPEN_THRESHOLD_OHMS 100000.0  // anything above this = treated as "open"

// Digital pins used for drive/ground control, one per wire (index 0-7 = wire 1-8)
const uint8_t wirePin[NUM_WIRES] = {2, 3, 4, 5, 6, 7, 8, 9};

// OLED setup
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_ADDR     0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Two ADS1115 boards
Adafruit_ADS1115 ads1; // addr 0x48 -> wires 1-4
Adafruit_ADS1115 ads2; // addr 0x49 -> wires 5-8

// Resistance matrix: result[i][j] = resistance reading when driving wire i,
// grounding wire j (we only use i<j, matrix is symmetric in practice)
float resultMatrix[NUM_WIRES][NUM_WIRES];

// Classification state
bool wireUsed[NUM_WIRES]; // marks wires already assigned to a coil/centertap

// ---------------- SETUP ----------------
void setup() {
  Serial.begin(9600);
  delay(300);
  Serial.println(F("=== 8-Wire Stepper Motor Tester ==="));

  Wire.begin();

  // Init both ADS1115 boards at their distinct addresses
  if (!ads1.begin(0x48)) {
    Serial.println(F("ERROR: ADS1115 #1 (0x48) not found!"));
  }
  if (!ads2.begin(0x49)) {
    Serial.println(F("ERROR: ADS1115 #2 (0x49) not found!"));
  }
  // +-6.144V range, good headroom for a 5V system
  ads1.setGain(GAIN_TWOTHIRDS);
  ads2.setGain(GAIN_TWOTHIRDS);

  // Init OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("ERROR: OLED not found!"));
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("Stepper Tester"));
  display.println(F("Ready. Connect"));
  display.println(F("motor + press"));
  display.println(F("any key/reset."));
  display.display();

  // All wire pins start as floating inputs (safe state)
  for (uint8_t i = 0; i < NUM_WIRES; i++) {
    pinMode(wirePin[i], INPUT);
  }

  Serial.println(F("Setup complete. Send any character to start test."));
}

// ---------------- MAIN LOOP ----------------
void loop() {
  if (Serial.available() > 0) {
    while (Serial.available()) Serial.read(); // clear buffer
    runFullTest();
  }
}

// ---------------- TEST SEQUENCE ----------------
void runFullTest() {
  Serial.println(F("\n--- Starting full 28-pair scan ---"));
  showMessage("Testing...", "Scanning 28", "wire pairs");

  // Clear matrix
  for (uint8_t i = 0; i < NUM_WIRES; i++)
    for (uint8_t j = 0; j < NUM_WIRES; j++)
      resultMatrix[i][j] = -1; // -1 = not yet measured / open

  // Test every unique pair (i,j), i<j -> 28 combinations for 8 wires
  for (uint8_t i = 0; i < NUM_WIRES; i++) {
    for (uint8_t j = i + 1; j < NUM_WIRES; j++) {
      float r = measurePair(i, j);
      resultMatrix[i][j] = r;
      resultMatrix[j][i] = r; // symmetric

      Serial.print(F("Wire "));
      Serial.print(i + 1);
      Serial.print(F(" <-> Wire "));
      Serial.print(j + 1);
      Serial.print(F(": "));
      if (r >= OPEN_THRESHOLD_OHMS) {
        Serial.println(F("OPEN"));
      } else {
        Serial.print(r, 1);
        Serial.println(F(" ohms"));
      }
    }
  }

  // All wires back to safe floating state
  for (uint8_t i = 0; i < NUM_WIRES; i++) pinMode(wirePin[i], INPUT);

  Serial.println(F("--- Scan complete ---\n"));
  classifyAndReport();
}

// Measure resistance between wire driveIdx and wire groundIdx.
// Returns resistance in ohms, or a large number (>= OPEN_THRESHOLD_OHMS) if open.
float measurePair(uint8_t driveIdx, uint8_t groundIdx) {
  // Float all wires first
  for (uint8_t k = 0; k < NUM_WIRES; k++) {
    pinMode(wirePin[k], INPUT);
  }

  // Drive one wire HIGH, ground the other
  pinMode(wirePin[driveIdx], OUTPUT);
  digitalWrite(wirePin[driveIdx], HIGH);
  pinMode(wirePin[groundIdx], OUTPUT);
  digitalWrite(wirePin[groundIdx], LOW);

  delay(SETTLE_TIME_MS);

  // Read voltage at the DRIVE wire's sense node (after its resistor)
  float voltage = readChannelVoltage(driveIdx);

  // Release pins back to floating
  pinMode(wirePin[driveIdx], INPUT);
  pinMode(wirePin[groundIdx], INPUT);

  // Voltage divider: Vsense = Vref * Rmotor / (Rmotor + Rref)
  // Solve for Rmotor:  Rmotor = Rref * Vsense / (Vref - Vsense)
  if (voltage >= (ADC_VREF - 0.02)) {
    // Essentially no drop across the reference resistor = open circuit
    return OPEN_THRESHOLD_OHMS + 1;
  }
  if (voltage <= 0.01) {
    // Dead short straight to ground with ~0 ohms (rare, but handle it)
    return 0.0;
  }

  float rMotor = REF_RESISTOR_OHMS * voltage / (ADC_VREF - voltage);
  return rMotor;
}

// Reads voltage at the sense node for a given wire index (0-7),
// routing to the correct ADS1115 board and channel.
float readChannelVoltage(uint8_t wireIdx) {
  int16_t raw;
  float voltage;

  if (wireIdx < 4) {
    raw = ads1.readADC_SingleEnded(wireIdx);
    voltage = ads1.computeVolts(raw);
  } else {
    raw = ads2.readADC_SingleEnded(wireIdx - 4);
    voltage = ads2.computeVolts(raw);
  }
  return voltage;
}

// ---------------- CLASSIFICATION ----------------
// Looks at the resistance matrix and tries to determine motor type:
// bipolar (4-wire), unipolar 5/6-wire (center tap), or full 8-wire.
void classifyAndReport() {
  for (uint8_t i = 0; i < NUM_WIRES; i++) wireUsed[i] = false;

  Serial.println(F("=== CLASSIFICATION ==="));

  // Count how many "live" (non-open) connections each wire has,
  // and find the connected-component groups (coils).
  // A coil group = set of wires that show continuity to each other.
  bool visited[NUM_WIRES] = {false};
  uint8_t numCoils = 0;
  uint8_t totalConnectedWires = 0;

  for (uint8_t i = 0; i < NUM_WIRES; i++) {
    if (visited[i]) continue;

    // Find all wires connected to wire i (a coil group, possibly with center tap)
    uint8_t group[NUM_WIRES];
    uint8_t groupSize = 0;
    for (uint8_t j = 0; j < NUM_WIRES; j++) {
      if (i == j) continue;
      if (resultMatrix[i][j] < OPEN_THRESHOLD_OHMS) {
        group[groupSize++] = j;
      }
    }

    if (groupSize == 0) {
      // wire i has no continuity to anything -> not connected / broken wire
      continue;
    }

    visited[i] = true;
    group[groupSize++] = i; // include self in the group list for reporting
    for (uint8_t g = 0; g < groupSize; g++) visited[group[g]] = true;

    numCoils++;
    totalConnectedWires += groupSize;

    reportCoilGroup(group, groupSize);
  }

  if (numCoils == 0) {
    Serial.println(F("No continuity detected on any wire pair."));
    Serial.println(F("Check connections and try again."));
    showMessage("No continuity", "found. Check", "connections.");
    return;
  }

  Serial.print(F("\nTotal coil groups found: "));
  Serial.println(numCoils);

  // High-level guess based on number of groups + wires per group
  String summary = "";
  if (numCoils == 2) {
    summary = "Likely 2-phase stepper (bipolar or unipolar)";
  } else if (numCoils == 4) {
    summary = "Likely 8-wire stepper (4 independent half-coils)";
  } else {
    summary = "Unusual pattern - verify wiring/connections";
  }
  Serial.println(summary);

  showMessage("Done. See", "Serial Monitor", "for full report");
}

// Reports details for one coil group: identifies center tap if present.
void reportCoilGroup(uint8_t *group, uint8_t size) {
  Serial.print(F("\nCoil group ("));
  Serial.print(size);
  Serial.print(F(" wires): "));
  for (uint8_t k = 0; k < size; k++) {
    Serial.print(group[k] + 1);
    if (k < size - 1) Serial.print(F(", "));
  }
  Serial.println();

  if (size == 2) {
    float r = resultMatrix[group[0]][group[1]];
    Serial.print(F("  Wire "));
    Serial.print(group[0] + 1);
    Serial.print(F(" <-> Wire "));
    Serial.print(group[1] + 1);
    Serial.print(F(": "));
    Serial.print(r, 1);
    Serial.println(F(" ohms (simple coil, no center tap)"));
    return;
  }

  if (size == 3) {
    // Likely a center-tapped coil: find the wire whose resistance to
    // each end is closest to half the end-to-end resistance.
    float rAB = resultMatrix[group[0]][group[1]];
    float rAC = resultMatrix[group[0]][group[2]];
    float rBC = resultMatrix[group[1]][group[2]];

    // The largest of the three readings is the full coil (end-to-end).
    // The wire NOT part of that largest pair is the center tap.
    uint8_t ctIdx;
    float fullR;
    if (rAB >= rAC && rAB >= rBC) {
      fullR = rAB; ctIdx = group[2];
    } else if (rAC >= rAB && rAC >= rBC) {
      fullR = rAC; ctIdx = group[1];
    } else {
      fullR = rBC; ctIdx = group[0];
    }

    Serial.print(F("  Full coil resistance: "));
    Serial.print(fullR, 1);
    Serial.println(F(" ohms"));
    Serial.print(F("  Center tap: Wire "));
    Serial.print(ctIdx + 1);
    Serial.print(F(" (approx "));
    Serial.print(fullR / 2.0, 1);
    Serial.println(F(" ohms to each end)"));
    Serial.println(F("  -> 5-wire unipolar style coil"));
    return;
  }

  if (size == 4) {
    // Could be a true 8-wire motor's two half-coils on one physical coil,
    // OR a 6-wire unipolar motor where both coils share leads oddly.
    // Print the full pairwise matrix for this group so the user can read it.
    Serial.println(F("  Pairwise resistances:"));
    for (uint8_t a = 0; a < size; a++) {
      for (uint8_t b = a + 1; b < size; b++) {
        Serial.print(F("    Wire "));
        Serial.print(group[a] + 1);
        Serial.print(F(" <-> Wire "));
        Serial.print(group[b] + 1);
        Serial.print(F(": "));
        Serial.print(resultMatrix[group[a]][group[b]], 1);
        Serial.println(F(" ohms"));
      }
    }
    Serial.println(F("  -> Review pattern: look for matching pairs"));
    Serial.println(F("     (two halves of a split coil) or a center tap."));
    return;
  }

  // size > 4 or unexpected grouping - dump raw matrix for manual review
  Serial.println(F("  Complex group - raw pairwise data:"));
  for (uint8_t a = 0; a < size; a++) {
    for (uint8_t b = a + 1; b < size; b++) {
      Serial.print(F("    Wire "));
      Serial.print(group[a] + 1);
      Serial.print(F(" <-> Wire "));
      Serial.print(group[b] + 1);
      Serial.print(F(": "));
      Serial.print(resultMatrix[group[a]][group[b]], 1);
      Serial.println(F(" ohms"));
    }
  }
}

// ---------------- OLED HELPER ----------------
void showMessage(const char *line1, const char *line2, const char *line3) {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println(line1);
  display.println(line2);
  display.println(line3);
  display.display();
}
