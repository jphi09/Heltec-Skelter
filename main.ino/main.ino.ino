#include <Arduino.h>
#include <HT_st7735.h>

// PIN DEFINITIONS
#define VGNSS_CTRL   3    // GPIO 3 → Vext
#define GPS_RX_PIN  33  
#define GPS_TX_PIN  34  
#define VBAT_PIN     A0   // ADC1_CH0 (junction of 100 Ω/390 Ω)
#define VBAT_EN       2   // Must be HIGH to connect divider
#define BL_CTRL_PIN  21   // TFT backlight

HT_st7735 st7735;

static int  gpsCount     = 0;
static int  glonassCount = 0;
static int  beidouCount  = 0;
static int  galileoCount = 0;
static int  qzssCount    = 0;
static int  totalInView  = 0;
static bool haveFix     = false;
static char lineBuf[128];
static int  linePos     = 0;
static unsigned long lastLCDupdate = 0;
static const unsigned long LCD_INTERVAL = 1000;

// Prototypes
void processNMEALine(const char* line);
int  parseGSVinView(const char* gsvLine);
int  parseGGAfixQuality(const char* ggaLine);
float readBatteryVoltageRaw(int &rawADC);
int   voltageToPercent(float vb);
float readCalibratedBat();       // new helper
void updateLCD();

void setup() {
  Serial.begin(115200);
  while (!Serial) { delay(10); }
  Serial.println();
  Serial.println("HTIT-Tracker v1.2 Calibrated Batt% Demo");

  pinMode(VBAT_EN, OUTPUT);
  digitalWrite(VBAT_EN, LOW);

  pinMode(VGNSS_CTRL, OUTPUT);
  digitalWrite(VGNSS_CTRL, LOW);
  Serial.println("→ VGNSS_CTRL=LOW (Vext ON)");
  delay(200);

  pinMode(BL_CTRL_PIN, OUTPUT);
  digitalWrite(BL_CTRL_PIN, HIGH);
  Serial.println("→ BL_CTRL=HIGH (Backlight ON)");

  analogSetPinAttenuation(VBAT_PIN, ADC_11db);

  Serial1.begin(115200, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("→ Serial1 started for UC6580");

  st7735.st7735_init();
  st7735.st7735_fill_screen(ST7735_BLACK);
}

void loop() {
  // A) NMEA parsing
  while (Serial1.available() > 0) {
    char c = (char)Serial1.read();
    Serial.write(c);
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

  // B) Once per second, update
  unsigned long now = millis();
  if (now - lastLCDupdate >= LCD_INTERVAL) {
    lastLCDupdate = now;

    // 1) Read raw ADC + VBAT
    int rawADC = 0;
    float vb     = readBatteryVoltageRaw(rawADC);

    // 2) Compute “calibrated VBAT” using 5.05× instead of 4.90×
    float vb_cal = (rawADC / 4095.0f) * 3.3f * 5.05f;

    // 3) Map calibrated VBAT → % (clamp 0–100)
    int pct_cal = voltageToPercent(vb_cal);

    // 4) Debug print
    static unsigned long lastPrint = 0;
    static bool firstTime = true;
    if (firstTime || (now - lastPrint >= 2000)) {
      firstTime = false;
      lastPrint = now;
      float vAD = (rawADC / 4095.0f) * 3.3f;
      Serial.print("Raw ADC = "); Serial.print(rawADC);
      Serial.print("    V_ADC = "); Serial.print(vAD, 3); Serial.print(" V");
      Serial.print("    VBAT = "); Serial.print(vb, 2); Serial.print(" V");
      Serial.print("    VBAT_cal = "); Serial.print(vb_cal, 2); Serial.print(" V");
      Serial.print("    Batt% = "); Serial.print(pct_cal); Serial.println(" %");
    }

    // 5) Update LCD using pct_cal
    //    (We’ll modify updateLCD to accept an external pct)
    char fixBuf[13];
    if (haveFix)       sprintf(fixBuf, "Fix: Yes    ");
    else               sprintf(fixBuf, "Fix: No     ");
    st7735.st7735_fill_screen(ST7735_BLACK);
    st7735.st7735_write_str(0, 0, String(fixBuf));

    char satBuf[13];
    sprintf(satBuf, "Sats:%3d    ", totalInView);
    st7735.st7735_write_str(0, 16, String(satBuf));

    char battBuf[13];
    sprintf(battBuf, "Batt:%3d%%    ", pct_cal);
    st7735.st7735_write_str(0, 32, String(battBuf));
  }
}

// Parse NMEA sentences
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
  totalInView = gpsCount + glonassCount + beidouCount + galileoCount + qzssCount;
}

int parseGSVinView(const char* gsvLine) {
  const char* p = strchr(gsvLine, ',');
  if (!p) return 0;
  p = strchr(p + 1, ','); if (!p) return 0;
  p = strchr(p + 1, ','); if (!p) return 0;
  return atoi(p + 1);
}

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

// Read raw ADC from the 100Ω/390Ω divider and compute true VBAT:
float readBatteryVoltageRaw(int &rawADC) {
  // Connect divider
  digitalWrite(VBAT_EN, HIGH);
  delayMicroseconds(10);
  rawADC = analogRead(VBAT_PIN);
  digitalWrite(VBAT_EN, LOW);

  const float ADC_MAX = 4095.0f;
  const float REF_V   = 3.3f;
  float vAD = (rawADC / ADC_MAX) * REF_V;

  // True VBAT = vAD × 4.90
  return vAD * 4.90f;
}

// Map VBAT (3.00 V–4.20 V) → 0–100 % by linear interpolation
int voltageToPercent(float vb) {
  if (vb >= 4.20f) {
    return 100;
  } 
  else if (vb >= 4.06f) {
    return (int)roundf(90.0f + (vb - 4.06f)/(4.20f - 4.06f)*10.0f);
  }
  else if (vb >= 3.98f) {
    return (int)roundf(80.0f + (vb - 3.98f)/(4.06f - 3.98f)*10.0f);
  }
  else if (vb >= 3.92f) {
    return (int)roundf(70.0f + (vb - 3.92f)/(3.98f - 3.92f)*10.0f);
  }
  else if (vb >= 3.87f) {
    return (int)roundf(60.0f + (vb - 3.87f)/(3.92f - 3.87f)*10.0f);
  }
  else if (vb >= 3.82f) {
    return (int)roundf(50.0f + (vb - 3.82f)/(3.87f - 3.82f)*10.0f);
  }
  else if (vb >= 3.79f) {
    return (int)roundf(40.0f + (vb - 3.79f)/(3.82f - 3.79f)*10.0f);
  }
  else if (vb >= 3.77f) {
    return (int)roundf(30.0f + (vb - 3.77f)/(3.79f - 3.77f)*10.0f);
  }
  else if (vb >= 3.74f) {
    return (int)roundf(20.0f + (vb - 3.74f)/(3.77f - 3.74f)*10.0f);
  }
  else if (vb >= 3.71f) {
    return (int)roundf(10.0f + (vb - 3.71f)/(3.74f - 3.71f)*10.0f);
  }
  else if (vb >= 3.30f) {
    return (int)roundf((vb - 3.30f)/(3.71f - 3.30f)*10.0f);
  }
  return 0;
}
