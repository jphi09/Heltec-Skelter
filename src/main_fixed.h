#ifndef MAIN_H
#define MAIN_H

#include <Arduino.h>
#include <HT_st7735.h>

// PIN DEFINITIONS
#define VGNSS_CTRL   3    // GPIO 3 → Vext (active-low) powers UC6580 + ST7735
#define GPS_RX_PIN  33    // UC6580 TX → ESP32 RX
#define GPS_TX_PIN  34    // UC6580 RX ← ESP32 TX
#define VBAT_PIN     A0   // ADC1_CH0 on GPIO 1 (junction of 100 Ω/390 Ω divider)
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
    float calculateBearingToHome();
    float calculateDistanceToHome();
    const char* getCardinalDirection(float bearingToHome);

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
};

// Implementation of HTITTracker class methods

inline HTITTracker::HTITTracker() 
    : gpsCount(0), glonassCount(0), beidouCount(0), galileoCount(0), 
      qzssCount(0), totalInView(0), haveFix(false), lastHDOP(99.99f),
      linePos(0), lastLCDupdate(0), homeEstablished(false),
      homeLat(0.0), homeLon(0.0), currentLat(0.0), currentLon(0.0),
      hasValidPosition(false) {
    memset(lineBuf, 0, sizeof(lineBuf));
}

inline void HTITTracker::begin() {
    // 1) USB-Serial for debugging
    Serial.begin(115200);
    while (!Serial) { delay(10); }
    Serial.println();
    Serial.println("HTIT-Tracker v1.2: 5-Row Display with Home Navigation");

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

        // 5) Draw five rows on the ST7735
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
    // Accurate lithium battery discharge curve for 3.7V 3000mAh battery
    // Based on typical 18650 lithium-ion discharge characteristics
    
    if (vb >= 4.20f) {
        return 100;  // Fully charged
    } 
    else if (vb >= 4.10f) {
        return (int)roundf(95.0f + (vb - 4.10f)/(4.20f - 4.10f)*5.0f);  // 95-100%
    }
    else if (vb >= 4.00f) {
        return (int)roundf(85.0f + (vb - 4.00f)/(4.10f - 4.00f)*10.0f); // 85-95%
    }
    else if (vb >= 3.90f) {
        return (int)roundf(70.0f + (vb - 3.90f)/(4.00f - 3.90f)*15.0f); // 70-85%
    }
    else if (vb >= 3.80f) {
        return (int)roundf(50.0f + (vb - 3.80f)/(3.90f - 3.80f)*20.0f); // 50-70%
    }
    else if (vb >= 3.70f) {
        return (int)roundf(30.0f + (vb - 3.70f)/(3.80f - 3.70f)*20.0f); // 30-50%
    }
    else if (vb >= 3.60f) {
        return (int)roundf(15.0f + (vb - 3.60f)/(3.70f - 3.60f)*15.0f); // 15-30%
    }
    else if (vb >= 3.50f) {
        return (int)roundf(5.0f + (vb - 3.50f)/(3.60f - 3.50f)*10.0f);  // 5-15%
    }
    else if (vb >= 3.30f) {
        return (int)roundf((vb - 3.30f)/(3.50f - 3.30f)*5.0f);          // 0-5%
    }
    return 0;  // Battery critically low or disconnected
}

// Home navigation helper methods implementation
inline float HTITTracker::calculateBearingToHome() {
    if (!homeEstablished || !hasValidPosition) {
        return 0.0f;  // Default to North
    }
    
    // Calculate bearing from current position to home
    double lat1 = currentLat * PI / 180.0;
    double lon1 = currentLon * PI / 180.0;
    double lat2 = homeLat * PI / 180.0;
    double lon2 = homeLon * PI / 180.0;
    
    double dLon = lon2 - lon1;
    
    double y = sin(dLon) * cos(lat2);
    double x = cos(lat1) * sin(lat2) - sin(lat1) * cos(lat2) * cos(dLon);
    
    double bearing = atan2(y, x) * 180.0 / PI;
    bearing = fmod(bearing + 360.0, 360.0);  // Normalize to 0-360
    
    return bearing;
}

inline float HTITTracker::calculateDistanceToHome() {
    if (!homeEstablished || !hasValidPosition) {
        return 0.0f;  // No distance if no home or position
    }
    
    // Calculate distance using Haversine formula
    double lat1 = currentLat * PI / 180.0;
    double lon1 = currentLon * PI / 180.0;
    double lat2 = homeLat * PI / 180.0;
    double lon2 = homeLon * PI / 180.0;
    
    double dLat = lat2 - lat1;
    double dLon = lon2 - lon1;
    
    double a = sin(dLat/2) * sin(dLat/2) + 
               cos(lat1) * cos(lat2) * 
               sin(dLon/2) * sin(dLon/2);
    double c = 2 * atan2(sqrt(a), sqrt(1-a));
    
    // Earth's radius in meters
    const double EARTH_RADIUS = 6371000.0;
    
    return EARTH_RADIUS * c;  // Distance in meters
}

inline const char* HTITTracker::getCardinalDirection(float bearingToHome) {
    if (!haveFix) {
        return "O";  // Show "O" when no GPS fix yet
    }
    
    if (!homeEstablished) {
        return "N";  // Point North when no home established but have fix
    }
    
    // Convert bearing to 8 cardinal directions
    if (bearingToHome >= 337.5 || bearingToHome < 22.5) {
        return "N";   // North
    } else if (bearingToHome >= 22.5 && bearingToHome < 67.5) {
        return "NE";  // Northeast
    } else if (bearingToHome >= 67.5 && bearingToHome < 112.5) {
        return "E";   // East
    } else if (bearingToHome >= 112.5 && bearingToHome < 157.5) {
        return "SE";  // Southeast
    } else if (bearingToHome >= 157.5 && bearingToHome < 202.5) {
        return "S";   // South
    } else if (bearingToHome >= 202.5 && bearingToHome < 247.5) {
        return "SW";  // Southwest
    } else if (bearingToHome >= 247.5 && bearingToHome < 292.5) {
        return "W";   // West
    } else {
        return "NW";  // Northwest
    }
}

inline void HTITTracker::updateLCD(int pct_cal) {
    // 1) Clear entire screen
    st7735.st7735_fill_screen(ST7735_BLACK);

    // 2) Row 0 (y=0): Fix status (8 chars) + Cardinal direction (4 chars)
    char fixBuf[16];
    if (haveFix) {
        sprintf(fixBuf, "Fix: Yes ");
    } else {
        sprintf(fixBuf, "Fix: No  ");
    }
    
    // Calculate bearing to home and get cardinal direction
    float bearingToHome = calculateBearingToHome();
    const char* direction = getCardinalDirection(bearingToHome);
    
    // Add direction to display string
    sprintf(fixBuf + 9, "%s", direction);
    st7735.st7735_write_str(0, 0, String(fixBuf));

    // 3) Row 1 (y=16): Distance to home in meters
    char distBuf[16];
    if (homeEstablished && hasValidPosition) {
        float distanceToHome = calculateDistanceToHome();
        if (distanceToHome < 1000) {
            sprintf(distBuf, "Home:%3.0fm   ", distanceToHome);
        } else {
            sprintf(distBuf, "Home:%3.1fkm  ", distanceToHome / 1000.0);
        }
    } else {
        sprintf(distBuf, "Home: --.-m   ");
    }
    st7735.st7735_write_str(0, 16, String(distBuf));

    // 4) Row 2 (y=32): Satellite count (12 chars)
    char satBuf[16];
    sprintf(satBuf, "Sats:%3d    ", totalInView);
    st7735.st7735_write_str(0, 32, String(satBuf));

    // 5) Row 3 (y=48): Battery percentage (12 chars)
    char battBuf[16];
    sprintf(battBuf, "Batt:%3d%%    ", pct_cal);
    st7735.st7735_write_str(0, 48, String(battBuf));

    // 6) Row 4 (y=64): Fix accuracy in meters (12 chars)
    float accuracy = lastHDOP * 5.0f;  // HDOP × 5 m
    char accBuf[16];
    if (haveFix && lastHDOP > 0.0f && lastHDOP < 100.0f) {
        sprintf(accBuf, "Acc:%4.1fm   ", accuracy);
    } else {
        sprintf(accBuf, "Acc: --.-m   ");
    }
    st7735.st7735_write_str(0, 64, String(accBuf));
}

#endif // MAIN_H
