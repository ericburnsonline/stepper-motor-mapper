# Stepper Motor Mapper

Identify unknown stepper motor wiring by measuring coil resistance across every wire pair - Arduino + ADS1115.

## What It Does

Pulled a stepper motor from old equipment with no datasheet and no idea which wires belong to which coil? This tool measures resistance between every combination of wires on a 4, 5, 6, or 8-wire stepper, then reports back which wires form coil pairs and which (if any) are center taps.

Instead of manually probing every wire combination with a multimeter (28 combinations for an 8-wire motor), the Arduino does it automatically and prints a classified result.

## Status

**⚠️ This project is AI-coded and has not yet been tested on real hardware.**

The firmware logic and circuit design were worked through in detail, but I haven't built and validated it on an actual stepper motor yet. I'll be testing soon and will update this README (and the code, if needed) once I've confirmed it works as designed. Treat this as a documented starting point, not a verified build - yet.

## Hardware

- Arduino Uno R3
- 2x ADS1115 16-bit ADC modules (I2C)
- SSD1306 or SSD1315 0.96" 128x64 OLED display (I2C)
- 8x 100Ω resistors
- Breadboard, jumper wires, and a way to clip onto the motor's wires

Full wiring details and a Fritzing diagram will be added once the build is tested and confirmed working.

## How It Works (Short Version)

1. The Arduino drives one wire to 5V (through a resistor) and grounds another, one pair at a time.
2. An ADS1115 channel reads the resulting voltage on the driven wire.
3. That voltage is converted to a resistance value using a voltage divider calculation.
4. After all wire pairs are tested, the firmware groups wires into coils and flags any center-tap wires it finds.
5. Results print to the Serial Monitor, with a short summary also shown on the OLED.

## Getting Started

1. Install the Arduino IDE.
2. Install these libraries via the Library Manager: `Adafruit ADS1X15`, `Adafruit SSD1306`, `Adafruit GFX Library`.
3. Wire up the hardware (diagram coming soon).
4. Upload `stepper_motor_mapper.ino`.
5. Open the Serial Monitor at 9600 baud, connect a motor, and send any character to start a scan.

## License

MIT - see [LICENSE](LICENSE).

## Related

Part of the build/testing process documented alongside other projects at [IoT Project Kit](https://iotprojectkit.com).
