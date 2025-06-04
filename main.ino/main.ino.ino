#include <Arduino.h>
#include <HT_st7735.h>

// ———————————————————————————————————————————————————————————
// PIN DEFINITIONS (Wireless Tracker v1.2)
// ———————————————————————————————————————————————————————————
#define VGNSS_CTRL   3    // Drives Vext (Active‐Low) → powers UC6580 + ST7735 
#define GPS_RX_PIN  33    // UC6580 TX → ESP32 RX
#define GPS_TX_PIN  34    // UC6580 RX ← ESP32 TX
#define VBAT_PIN     A0   // ADC1_CH0 (through 2:1 divider) 

// Backlight enable for ST7735: must be HIGH to see the screen 
#define BL_CTRL_PIN 21

// Instantiate the HT_st7735 object (SPI pins are hardwired on v1.2)
HT_st7735 st7735;

// ———————————————————————————————————————————————————————————
// “Satellites In View” counts per constellation
// ———————————————————————————————————————————————————————————
static int gpsCount     = 0;
static int glonassCount = 0;
static int beidouCount  = 0;
static int galileoCount = 0;
static int qzssCount    = 0;

// Total satellites in view
static int totalInView  = 0;

// Whether we have a valid fix
static bool haveFix     = false;

// Buffer for one NMEA line at a time
static char lineBuf[128];
static int  linePos     = 0;

// For timing the LCD update (once per second)
static unsigned long lastLCDupdate = 0;
static const unsigned long LCD_INTERVAL = 1000; // 1000 ms = 1 second

// ———————————————————————————————————————————————————————————
// FUNCTION PROTOTYPES
// ———————————————————————————————————————————————————————————
void processNMEALine(const char* line);
int  parseGSVinView(const char* gsvLine);
int  parseGGAfixQuality(const char* ggaLine);
float readBatteryVoltage();
void updateLCD();


// ———————————————————————————————————————————————————————————
// SETUP()
// ———————————————————————————————————————————————————————————
void setup() {
  // 1) USB-Serial for debugging
  Serial.begin(115200);
  while (!Serial) {
    delay(10);
  }
  Serial.println();
  Serial.println("Starting raw-NMEA + LCD 3-row display…");

  // 2) Power on GNSS + TFT via Vext (active-low on v1.2)
  pinMode(VGNSS_CTRL, OUTPUT);
  digitalWrite(VGNSS_CTRL, LOW);  // Enable Vext = 3.3V → UC6580 + ST7735 
  Serial.println("→ VGNSS_CTRL (GPIO 3) = LOW (GNSS + TFT powered)");
  delay(200); // let regulator + GNSS chip stabilize

  // 3) Turn ON TFT backlight
  pinMode(BL_CTRL_PIN, OUTPUT);
  digitalWrite(BL_CTRL_PIN, HIGH);  // Illuminate ST7735 backlight 
  Serial.println("→ BL_CTRL (GPIO 21) = HIGH (Backlight ON)");

  // 4) Initialize Serial1 @ 115200 to read UC6580 NMEA
  Serial1.begin(115200, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("→ Serial1.begin(115200, RX=33, TX=34) for UC6580");
  Serial.println("Echoing raw NMEA + parsing sats/fix…");

  // 5) Initialize the ST7735 display
  st7735.st7735_init();                                                   // Heltec’s default SPI pins :contentReference[oaicite:12]{index=12}
  st7735.st7735_fill_screen(ST7735_BLACK);                                 // Clear screen

  // We do NOT draw static labels here. Instead, we will dynamically paint all three rows.
  // That way, each update can simply fill_screen + repaint:
  //   - Row 0 (y=0)   → Fix status
  //   - Row 1 (y=8)   → Sats count
  //   - Row 2 (y=16)  → Battery voltage
}


// ———————————————————————————————————————————————————————————
// LOOP()
// ———————————————————————————————————————————————————————————
void loop() {
  // A) Read raw NMEA from Serial1, echo to USB-Serial, accumulate into lineBuf
  while (Serial1.available() > 0) {
    char c = (char)Serial1.read();
    Serial.write(c);  // Echo raw NMEA to Serial Monitor

    if (c == '\r' || c == '\n') {
      if (linePos > 0) {
        lineBuf[linePos] = '\0';
        processNMEALine(lineBuf);
        linePos = 0;
      }
    }
    else if (linePos < (int)sizeof(lineBuf) - 1) {
      lineBuf[linePos++] = c;
    }
  }

  // B) Once per second, redraw all three rows
  unsigned long now = millis();
  if (now - lastLCDupdate >= LCD_INTERVAL) {
    lastLCDupdate = now;
    updateLCD();
  }
}


// ———————————————————————————————————————————————————————————
// processNMEALine(line):
//   Parses one full NMEA sentence to update satellite counts and fix flag.
// ———————————————————————————————————————————————————————————
void processNMEALine(const char* line) {
  if (strncmp(line, "$GPGSV", 6) == 0) {
    gpsCount = parseGSVinView(line);
  }
  else if (strncmp(line, "$GLGSV", 6) == 0) {
    glonassCount = parseGSVinView(line);
  }
  else if (strncmp(line, "$GBGSV", 6) == 0) {
    beidouCount = parseGSVinView(line);
  }
  else if (strncmp(line, "$GAGSV", 6) == 0) {
    galileoCount = parseGSVinView(line);
  }
  else if (strncmp(line, "$GQGSV", 6) == 0) {
    qzssCount = parseGSVinView(line);
  }
  else if (strncmp(line, "$GNGGA", 6) == 0) {
    haveFix = (parseGGAfixQuality(line) > 0);
  }

  // Sum up total satellites in view
  totalInView = gpsCount +
                glonassCount +
                beidouCount +
                galileoCount +
                qzssCount;
}


// ———————————————————————————————————————————————————————————
// parseGSVinView(gsvLine):
//   Extract the “totalInView” (4th field) from a $GxGSV sentence.
// ———————————————————————————————————————————————————————————
int parseGSVinView(const char* gsvLine) {
  // Format: "$GxGSV,<numMsgs>,<msgNum>,<totalInView>,…"
  const char* p = strchr(gsvLine, ',');
  if (!p) return 0;
  p = strchr(p + 1, ','); if (!p) return 0;
  p = strchr(p + 1, ','); if (!p) return 0;
  return atoi(p + 1);
}


// ———————————————————————————————————————————————————————————
// parseGGAfixQuality(ggaLine):
//   Extract the 7th field (<fixQuality>) from a $GNGGA sentence.
// ———————————————————————————————————————————————————————————
int parseGGAfixQuality(const char* ggaLine) {
  int commaCount = 0;
  const char* p = ggaLine;
  while (*p && commaCount < 6) {
    if (*p == ',') commaCount++;
    p++;
  }
  if (!*p) return 0;
  return atoi(p);
}


// ———————————————————————————————————————————————————————————
// readBatteryVoltage():
//   Reads ADC at VBAT_PIN (via 2:1 resistor divider) and returns VBAT.
// ———————————————————————————————————————————————————————————
float readBatteryVoltage() {
  const float ADC_MAX        = 4095.0f;   // 12-bit ADC 
  const float REF_VOLT       = 3.3f;      // ESP32 ADC reference
  const float DIVIDER_FACTOR = 2.0f;      // VBAT_PIN = VBAT / 2

  int raw = analogRead(VBAT_PIN);
  float vAD = (raw / ADC_MAX) * REF_VOLT;
  return vAD * DIVIDER_FACTOR;
}


// ———————————————————————————————————————————————————————————
// updateLCD():
//   Clears the entire screen, then draws exactly three rows of text:
//     y=0   : “Fix: Yes” or “Fix: No”
//     y=8   : “Sats: XX”
//     y=16  : “Batt: X.X V”
//   All in the default 8×8 px font provided by HT_st7735.
// ———————————————————————————————————————————————————————————
void updateLCD() {
  // 1) Clear entire screen to black
  st7735.st7735_fill_screen(ST7735_BLACK);                       // HT_st7735 clear 

  // 2) First row (y=0): Fix status (fixed-width, total length = 12 chars)
  char fixBuf[13];
  if (haveFix) {
    // “Fix: Yes” + 4 spaces = 12 chars
    sprintf(fixBuf, "Fix: Yes    ");
  } else {
    // “Fix: No ” + 5 spaces = 12 chars (extra space after “No”)
    sprintf(fixBuf, "Fix: No     ");
  }
  st7735.st7735_write_str(0, 0, String(fixBuf));                 // Draw at (x=0, y=0)

  // 3) Second row (y=8): Satellites count, padded to 12 chars
  char satBuf[13];
  // Use %3d so up to 3-digit satellite counts still align, then pad spaces
  sprintf(satBuf, "Sats:%3d    ", totalInView);                 // Always 12 chars long
  st7735.st7735_write_str(0, 16, String(satBuf));                 // Draw at (x=0, y=8)

  // 4) Third row (y=16): Battery voltage, formatted “Batt: X.X V” + spaces
  float vb = readBatteryVoltage();
  char battBuf[13];
  // Format with one decimal, e.g. “Batt: 3.7 V” (8 chars) + 4 spaces → total 12
  sprintf(battBuf, "Batt:%4.1fV   ", vb);
  st7735.st7735_write_str(0, 32, String(battBuf));               // Draw at (x=0, y=16)
}

