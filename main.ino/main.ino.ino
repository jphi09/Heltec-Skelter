#include <Arduino.h>
#include <HT_st7735.h>

// Pin definitions (per your board)
#define VGNSS_CTRL   3   // GPIO that powers the UC6580
#define GPS_RX_PIN  33   // UC6580 TX → ESP32 RX
#define GPS_TX_PIN  34   // UC6580 RX ← ESP32 TX

HT_st7735 st7735;

// “Satellites In View” counts per constellation
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
static const unsigned long LCD_INTERVAL = 1000; // 1000 ms = 1 s

void setup() {
  // 1) USB serial for debugging
  Serial.begin(115200);
  while (!Serial) {
    delay(10);
  }
  Serial.println();
  Serial.println("Starting raw‐NMEA + LCD sat/fix display…");

  // 2) Power on UC6580 via GPIO 3
  pinMode(VGNSS_CTRL, OUTPUT);
  digitalWrite(VGNSS_CTRL, HIGH);
  Serial.println("→ VGNSS_CTRL (GPIO 3) = HIGH (UC6580 powered)");
  delay(200); // let the GNSS chip spin up

  // 3) Initialize Serial1 @ 115200 (RX=33, TX=34) to read UC6580
  Serial1.begin(115200, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("→ Serial1.begin(115200, RX=33, TX=34) for UC6580");
  Serial.println("Echoing raw NMEA + parsing sats/fix…");

  // 4) Initialize ST7735 display once and draw static labels
  st7735.st7735_init();
  st7735.st7735_fill_screen(ST7735_BLACK);

  // Draw static labels on their own 8-pixel rows:
  //   y=0   : “Satellites In View:”
  //   y=16  : “Fix Status:”
  st7735.st7735_write_str(0,  0, (String)"Satellites In View:");
  st7735.st7735_write_str(0, 16, (String)"Fix Status:");

  // Draw initial placeholders for dynamic lines, also padded to 12 chars:
  //   y=8   : “Sats: --      ”
  //   y=24  : “Fix: ---      ”
  st7735.st7735_write_str(0,  8, (String)"Sats: --      ");
  st7735.st7735_write_str(0, 24, (String)"Fix: ---      ");
}

void loop() {
  // A) Read bytes from UC6580 and accumulate into lineBuf
  while (Serial1.available() > 0) {
    char c = (char)Serial1.read();
    // Echo raw NMEA to Serial Monitor
    Serial.write(c);

    // Accumulate until newline or carriage return
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

  // B) Only update the LCD once per second
  unsigned long now = millis();
  if (now - lastLCDupdate >= LCD_INTERVAL) {
    lastLCDupdate = now;
    updateLCD();
  }
}

// Parses one NMEA sentence (line) to update satellite counts and fix flag
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
  totalInView = gpsCount + glonassCount + beidouCount + galileoCount + qzssCount;
}

// Extracts the “total satellites in view” (4th field) from a GxGSV sentence
int parseGSVinView(const char* gsvLine) {
  // "$GxGSV,<numMsgs>,<msgNum>,<totalInView>,..."
  const char* p = strchr(gsvLine, ',');
  if (!p) return 0;
  p = strchr(p + 1, ',');
  if (!p) return 0;
  p = strchr(p + 1, ',');
  if (!p) return 0;
  return atoi(p + 1);
}

// Extracts the fix quality (7th field) from a GNGGA sentence
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

// Writes only the two dynamic lines in fixed‐width form so they fully overwrite
void updateLCD() {
  // 1) “Sats: XX” at y=8, padded to 12 characters
  //    We use two digits and then spaces so old “XX” will be fully erased.
  char satBuf[13];
  sprintf(satBuf, "Sats: %2d      ", totalInView); // always length 12
  st7735.st7735_write_str(0,  8, String(satBuf));

  // 2) “Fix: Yes” or “Fix: No” at y=24, padded to 12 characters
  char fixBuf[13];
  if (haveFix) {
    // “Fix: Yes” + 5 spaces = 12 chars total
    sprintf(fixBuf, "Fix: Yes      ");
  } else {
    // “Fix: No ” + 5 spaces = 12 chars total (extra space after “No”)
    sprintf(fixBuf, "Fix: No       ");
  }
  st7735.st7735_write_str(0, 24, String(fixBuf));
}
