#ifndef MAIN_H
#define MAIN_H

#include <Arduino.h>
#include <HT_st7735.h>

// PIN DEFINITIONS
#define VGNSS_CTRL   3    // GPIO 3 → Vext (active-low) powers UC6580 + ST7735
#define GPS_RX_PIN  33    // UC6580 TX → ESP32 RX
#define GPS_TX_PIN  34    // UC6580 RX ← ESP32inline float HTITTracker::parseGGAHDOP(const char* ggaLine) {
    // Extract 9th field (<HDOP>) from "$GNGGA"
    int commaCount = 0;
    const char* p = ggaLine;
    while (*p && commaCount < 8) {
        if (*p == ',') commaCount++;
        p++;
    }
    if (!*p || *p == ',') return -1.0f;
    return atof(p);
}

inline bool HTITTracker::parseGGAPosition(const char* ggaLine, double &lat, double &lon) {
    // Parse $GNGGA format: $GNGGA,time,lat,N/S,lon,E/W,fix,sats,hdop,alt,M,geoid,M,dgps_age,dgps_id*checksum
    // Fields: 0=GNGGA, 1=time, 2=lat, 3=N/S, 4=lon, 5=E/W, 6=fix_quality
    
    const char* fields[15];
    int fieldCount = 0;
    const char* start = ggaLine;
    
    // Split the line by commas
    for (const char* p = ggaLine; *p && fieldCount < 15; p++) {
        if (*p == ',' || *p == '*') {
            fields[fieldCount++] = start;
            start = p + 1;
        }
    }
    
    if (fieldCount < 6) return false;
    
    // Parse latitude (field 2, format: ddmm.mmmmm)
    const char* latStr = fields[2];
    const char* latDir = fields[3];
    if (strlen(latStr) < 7 || strlen(latDir) < 1) return false;
    
    double latDeg = (latStr[0] - '0') * 10 + (latStr[1] - '0');
    double latMin = atof(latStr + 2);
    lat = latDeg + latMin / 60.0;
    if (latDir[0] == 'S') lat = -lat;
    
    // Parse longitude (field 4, format: dddmm.mmmmm)
    const char* lonStr = fields[4];
    const char* lonDir = fields[5];
    if (strlen(lonStr) < 8 || strlen(lonDir) < 1) return false;
    
    double lonDeg = (lonStr[0] - '0') * 100 + (lonStr[1] - '0') * 10 + (lonStr[2] - '0');
    double lonMin = atof(lonStr + 3);
    lon = lonDeg + lonMin / 60.0;
    if (lonDir[0] == 'W') lon = -lon;
    
    return true;
}e VBAT_PIN     A0   // ADC1_CH0 on GPIO 1 (junction of 100 Ω/390 Ω divider)
#define VBAT_EN       2   // GPIO 2 must be HIGH to connect that divider
#define BL_CTRL_PIN  21   // GPIO 21 enables ST7735 backlight (HIGH = on)

class HTITTracker {
private:
    // Display instance
    HT_st7735 st7735;
    
    // Satellite counts per constellation
    int gpsCount;
    int glonassCount;
    int beidouCount;
    int galileoCount;
    int qzssCount;
    int totalInView;
    
    // GNSS fix flag and HDOP → accuracy
    bool haveFix;
    float lastHDOP;
    
    // Home navigation variables
    bool homeEstablished;
    double homeLat, homeLon;           // Home coordinates
    double currentLat, currentLon;     // Current coordinates
    bool hasValidPosition;             // Current position is valid
    
    // Movement tracking for direction calculation
    struct PositionHistory {
        double lat, lon;
        unsigned long timestamp;
        bool valid;
    };
    static const int HISTORY_SIZE = 5;
    PositionHistory posHistory[HISTORY_SIZE];
    int historyIndex;
    
    // Current heading (0-359 degrees, 0=North)
    float currentHeading;
    bool hasValidHeading;
    
    // Buffer for NMEA line accumulation
    char lineBuf[128];
    int linePos;
    
    // Timing for LCD refresh (once per second)
    unsigned long lastLCDupdate;
    static const unsigned long LCD_INTERVAL = 1000; // ms
    
    // Private helper methods
    void processNMEALine(const char* line);
    int parseGSVinView(const char* gsvLine);
    int parseGGAfixQuality(const char* ggaLine);
    float parseGGAHDOP(const char* ggaLine);
    bool parseGGAPosition(const char* ggaLine, double &lat, double &lon);
    float readBatteryVoltageRaw(int &rawADC);
    int voltageToPercent(float vb);
    void updateLCD(int pct_cal);
    
    // Home navigation helper methods
    void updatePositionHistory(double lat, double lon);
    void calculateHeadingFromMovement();
    float calculateBearingToHome();
    char getDirectionArrow(float bearingToHome);

public:
    // Constructor
    HTITTracker();
    
    // Public methods
    void begin();
    void update();
    
    // Getters for status information
    bool getFixStatus() const { return haveFix; }
    int getTotalSatellites() const { return totalInView; }
    float getHDOP() const { return lastHDOP; }
    int getGPSCount() const { return gpsCount; }
    int getGLONASSCount() const { return glonassCount; }
    int getBeidouCount() const { return beidouCount; }
    int getGalileoCount() const { return galileoCount; }
    int getQZSSCount() const { return qzssCount; }
    
    // Home navigation getters
    bool isHomeEstablished() const { return homeEstablished; }
    bool hasCurrentPosition() const { return hasValidPosition; }
    float getCurrentHeading() const { return currentHeading; }
};

// Implementation of HTITTracker class methods

inline HTITTracker::HTITTracker() 
    : gpsCount(0), glonassCount(0), beidouCount(0), galileoCount(0), 
      qzssCount(0), totalInView(0), haveFix(false), lastHDOP(99.99f),
      linePos(0), lastLCDupdate(0), homeEstablished(false),
      homeLat(0.0), homeLon(0.0), currentLat(0.0), currentLon(0.0),
      hasValidPosition(false), historyIndex(0), currentHeading(0.0f),
      hasValidHeading(false) {
    memset(lineBuf, 0, sizeof(lineBuf));
    // Initialize position history
    for (int i = 0; i < HISTORY_SIZE; i++) {
        posHistory[i].valid = false;
        posHistory[i].lat = 0.0;
        posHistory[i].lon = 0.0;
        posHistory[i].timestamp = 0;
    }
}

inline void HTITTracker::begin() {
    // 1) USB-Serial for debugging
    Serial.begin(115200);
    while (!Serial) { delay(10); }
    Serial.println();
    Serial.println("HTIT-Tracker v1.2: 4-Row Display (Fix, Sats, Batt%, Acc)");

    // 2) Configure VBAT_EN (GPIO 2) and keep LOW until measurement
    pinMode(VBAT_EN, OUTPUT);
    digitalWrite(VBAT_EN, LOW);

    // 3) Power on GNSS + TFT via Vext (active-low on v1.2)
    pinMode(VGNSS_CTRL, OUTPUT);
    digitalWrite(VGNSS_CTRL, LOW);   // Enable 3.3 V rail for UC6580 + ST7735
    Serial.println("→ VGNSS_CTRL (GPIO 3) = LOW (GNSS + TFT powered)");
    delay(200);  // allow regulator + GNSS to stabilize

    // 4) Enable TFT backlight
    pinMode(BL_CTRL_PIN, OUTPUT);
    digitalWrite(BL_CTRL_PIN, HIGH);  // Turn backlight ON
    Serial.println("→ BL_CTRL (GPIO 21) = HIGH (Backlight ON)");

    // 5) Set ADC attenuation so VBAT/2 (≈0.857–1.07 V) reads accurately
    analogSetPinAttenuation(VBAT_PIN, ADC_11db);

    // 6) Initialize Serial1 @115200 to read UC6580 NMEA
    Serial1.begin(115200, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    Serial.println("→ Serial1.begin(115200, RX=33, TX=34) for UC6580");

    // 7) Initialize ST7735 display
    st7735.st7735_init();
    st7735.st7735_fill_screen(ST7735_BLACK);
}

inline void HTITTracker::update() {
    // A) Read raw NMEA from Serial1, echo to USB-Serial, accumulate lines
    while (Serial1.available() > 0) {
        char c = (char)Serial1.read();
        Serial.write(c);  // echo raw NMEA
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

    // B) Once per second, update display
    unsigned long now = millis();
    if (now - lastLCDupdate >= LCD_INTERVAL) {
        lastLCDupdate = now;

        // 1) Read raw ADC + true VBAT (volts)
        int rawADC = 0;
        float vb = readBatteryVoltageRaw(rawADC);

        // 2) Compute "calibrated VBAT" using 5.05× instead of 4.90×
        float vb_cal = (rawADC / 4095.0f) * 3.3f * 5.05f;

        // 3) Map calibrated VBAT → 0–100% clamped
        int pct_cal = voltageToPercent(vb_cal);

        // 4) Debug print every 2 s
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

        // 5) Draw four rows on the ST7735
        updateLCD(pct_cal);
    }
}

inline void HTITTracker::processNMEALine(const char* line) {
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
        // 1) Fix quality (7th field)
        int fixQual = parseGGAfixQuality(line);
        haveFix = (fixQual > 0);

        // 2) HDOP (9th field) → update lastHDOP if valid
        float hdop = parseGGAHDOP(line);
        if (hdop > 0.0f && hdop < 100.0f) {
            lastHDOP = hdop;
        }
        
        // 3) Parse position (latitude and longitude)
        double lat, lon;
        if (parseGGAPosition(line, lat, lon)) {
            currentLat = lat;
            currentLon = lon;
            hasValidPosition = true;
            
            // Update position history for movement tracking
            updatePositionHistory(lat, lon);
            
            // Calculate heading from movement
            calculateHeadingFromMovement();
            
            // Establish home if we have a fix and haven't set home yet
            if (haveFix && !homeEstablished) {
                homeLat = lat;
                homeLon = lon;
                homeEstablished = true;
                Serial.println("→ HOME ESTABLISHED!");
                Serial.print("   Home coordinates: ");
                Serial.print(homeLat, 6); Serial.print(", "); Serial.println(homeLon, 6);
            }
        }
    }
    // Sum satellites in view
    totalInView = gpsCount + glonassCount + beidouCount + galileoCount + qzssCount;
}

inline int HTITTracker::parseGSVinView(const char* gsvLine) {
    // Extract 4th field (<totalInView>) from "$GxGSV"
    const char* p = strchr(gsvLine, ',');
    if (!p) return 0;
    p = strchr(p + 1, ','); if (!p) return 0;
    p = strchr(p + 1, ','); if (!p) return 0;
    return atoi(p + 1);
}

inline int HTITTracker::parseGGAfixQuality(const char* ggaLine) {
    // Extract 7th field (<fixQuality>) from "$GNGGA"
    int commaCount = 0;
    const char* p = ggaLine;
    while (*p && commaCount < 6) {
        if (*p == ',') commaCount++;
        p++;
    }
    if (!*p) return 0;
    return atoi(p);
}

inline float HTITTracker::parseGGAHDOP(const char* ggaLine) {
    // Extract 9th field (<HDOP>) from "$GNGGA"
    int commaCount = 0;
    const char* p = ggaLine;
    while (*p && commaCount < 8) {
        if (*p == ',') p++;
        p++;
    }
    if (!*p || *p == ',') return -1.0f;
    return atof(p);
}

inline float HTITTracker::readBatteryVoltageRaw(int &rawADC) {
    // 1) Drive VBAT_EN HIGH to connect 100 Ω/390 Ω divider
    digitalWrite(VBAT_EN, HIGH);
    delayMicroseconds(10);

    // 2) Read 12-bit ADC (0–4095)
    rawADC = analogRead(VBAT_PIN);

    // 3) Disconnect divider to save battery
    digitalWrite(VBAT_EN, LOW);

    // 4) Convert raw→V_ADC in volts
    const float ADC_MAX = 4095.0f;
    const float REF_V = 3.3f;
    float vAD = (rawADC / ADC_MAX) * REF_V;

    // 5) Return true VBAT = vAD × 4.90 (because divider = 100/(100+390))
    return vAD * 4.90f;
}

inline int HTITTracker::voltageToPercent(float vb) {
    // Map VBAT (3.00–4.20 V) → 0–100 % by linear interpolation
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

inline void HTITTracker::updateLCD(int pct_cal) {
    // 1) Clear entire screen
    st7735.st7735_fill_screen(ST7735_BLACK);

    // 2) Row 0 (y=0): Fix status (12 chars)
    char fixBuf[16];
    if (haveFix) {
        sprintf(fixBuf, "Fix: Yes    ");
    } else {
        sprintf(fixBuf, "Fix: No     ");
    }
    st7735.st7735_write_str(0, 0, String(fixBuf));

    // 3) Row 1 (y=16): Satellite count (12 chars)
    char satBuf[16];
    sprintf(satBuf, "Sats:%3d    ", totalInView);
    st7735.st7735_write_str(0, 16, String(satBuf));

    // 4) Row 2 (y=32): Battery percentage (12 chars)
    char battBuf[16];
    sprintf(battBuf, "Batt:%3d%%    ", pct_cal);
    st7735.st7735_write_str(0, 32, String(battBuf));

    // 5) Row 3 (y=48): Fix accuracy in meters (12 chars)
    float accuracy = lastHDOP * 5.0f;  // HDOP × 5 m
    char accBuf[16];
    if (haveFix && lastHDOP > 0.0f && lastHDOP < 100.0f) {
        sprintf(accBuf, "Acc:%4.1fm   ", accuracy);
    } else {
        sprintf(accBuf, "Acc: --.-m   ");
    }
    st7735.st7735_write_str(0, 48, String(accBuf));
}

#endif // MAIN_H
