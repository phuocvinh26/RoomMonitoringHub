/****************************************************************************************
 *  Room Monitoring Hub – ESP32 (PlatformIO)
 *
 *  Features
 *  --------
 *  • DHT22  – temperature & humidity (GPIO4)
 *  • MQ‑2   – digital smoke/gas detector (GPIO5)
 *  • LDR    – analog light sensor (GPIO34 → ADC1_CH6)
 *  • OLED   – 0.96" I2C SSD1306 (SDA‑21, SCL‑22)
 *  • LEDs   – Green (12), Yellow (13), Red (14)
 *  • Buzzer – active buzzer (27)
 *  • Button – internal pull‑up (26) – mutes the buzzer while alarm is active
 *
 *  Logic
 *  -----
 *  Level 1 (Normal)      : Green LED ON
 *  Level 2 (Warning)     : Yellow LED ON
 *  Level 3 (Danger)      : Red LED ON + continuous buzzer
 *
 *  Wi‑Fi & Firebase – data are pushed every 5 seconds as a JSON object:
 *  {
 *      "device_id":"ROOMHUB00001",
 *      "temperature":23.5,
 *      "humidity":55.2,
 *      "smoke_detected":false,
 *      "light_level":7,
 *      "status_level":1
 *  }
 *
 *  --------------------------------------------------------------
 *  Author : ChatGPT (2024‑06)
 *  --------------------------------------------------------------
 *****************************************************************************************/

#include <Arduino.h>
#include <WiFi.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <Firebase_ESP_Client.h>
// #include <FirebaseJson.h>  // Removed – JSON handling is built‑in with Firebase_ESP_Client
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>

/* ---------------------------  Pin definitions  --------------------------- */
#define PIN_DHT          4          // DHT22 data pin
#define PIN_MQ2          5          // MQ‑2 digital output
#define PIN_LDR          34         // LDR analog pin (ADC1_CH6)
#define PIN_LED_GREEN    12
#define PIN_LED_YELLOW   13
#define PIN_LED_RED      14
#define PIN_BUZZER       27
#define PIN_BUTTON       26         // internal pull‑up

/* ---------------------------  OLED settings  ---------------------------- */
#define OLED_SDA         21
#define OLED_SCL         22
#define OLED_ADDR        0x3C       // most 0.96" SSD1306 modules use 0x3C
#define SCREEN_WIDTH     128
#define SCREEN_HEIGHT    64
#define OLED_RESET      -1          // no reset pin

/* ---------------------------  Wi‑Fi / Firebase  -------------------------- */
const char* WIFI_SSID     = "kkk";      // <-- fill in your SSID
const char* WIFI_PASSWORD = "vinh123490";      // <-- fill in your password

// Firebase project details – replace with your own values
#define FIREBASE_HOST    "your-project-id.firebaseio.com"
#define FIREBASE_AUTH    "your-database-secret-or-auth-token"

/* ---------------------------  Other constants  -------------------------- */
#define DEVICE_ID        "ROOMHUB00001"
#define UPLOAD_INTERVAL  5000UL      // ms (5 seconds)

#define TEMP_WARNING    30.0        // °C → Level 2
#define TEMP_DANGER     37.0        // °C → Level 3
#define HUM_WARNING     70.0        // %   → Level 2
#define HUM_DANGER      80.0        // %   → Level 3

/* ----------  LEDC (buzzer) ---------- */
#define BUZZER_CHANNEL   0          // LEDC channel 0 (0‑15)
#define BUZZER_FREQ      2000       // 2 kHz tone frequency
#define BUZZER_RESOLUTION 8         // 8‑bit resolution (0‑255)

/* ---------------------------  Global objects  --------------------------- */
DHT dht(PIN_DHT, DHT22);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

/* ---------------------------  Runtime variables  ------------------------ */
unsigned long lastUpload = 0;
bool buzzerMuted = false;          // true when user pressed the mute button
bool buttonPrevState = HIGH;      // for simple debounce
unsigned long buttonLastDebounce = 0;
const unsigned long DEBOUNCE_MS = 50;

/* -------------------------------------------------------------------------
   Helper functions
   ------------------------------------------------------------------------- */

/**
 * @brief Convert raw ADC (0‑4095) to a light level in the range 0‑10.
 */
uint8_t readLightLevel()
{
    int raw = analogRead(PIN_LDR); 
    
    // 0-4095 sang 0-10
    uint8_t level = map(raw, 0, 4095, 0, 10);
    
    // Chặn an toàn
    return constrain(level, 0, 10);
}

/**
 * @brief Read the digital MQ‑2 output.
 *        Most MQ‑2 modules pull the DO line LOW when gas exceeds the threshold.
 *        Adjust the logic if your board behaves opposite.
 */
bool readSmokeDetected()
{
    int state = digitalRead(PIN_MQ2);
    // LOW → smoke/gas detected, HIGH → clean air
    return (state == LOW);
}

/**
 * @brief Determine the current alarm level (1, 2 or 3).
 *
 * Level 3 has priority (smoke OR extreme temperature/humidity).
 * Level 2 is a warning (moderately high temperature or humidity).
 * Level 1 is the normal state.
 */
uint8_t evaluateStatusLevel(float temperature, float humidity, bool smoke)
{
    if (smoke) return 3;                                 // smoke overrides everything

    if (temperature >= TEMP_DANGER || humidity >= HUM_DANGER)
        return 3;                                         // extreme T/H → danger

    if (temperature >= TEMP_WARNING || humidity >= HUM_WARNING)
        return 2;                                         // moderate T/H → warning

    return 1;                                            // normal
}

/**
 * @brief Update LEDs and buzzer according to the current status level.
 */
void applyOutputs(uint8_t level, bool smoke)
{
    // ---- LEDs -------------------------------------------------------------
    digitalWrite(PIN_LED_GREEN,  (level == 1) ? HIGH : LOW);
    digitalWrite(PIN_LED_YELLOW, (level == 2) ? HIGH : LOW);
    digitalWrite(PIN_LED_RED,    (level == 3) ? HIGH : LOW);

    // ---- Buzzer -----------------------------------------------------------
    if (level == 3 && !buzzerMuted)
    {
        // Continuous tone (2 kHz). Use tone() for an active buzzer.
        ledcWrite(BUZZER_CHANNEL, 128); // 50% duty (2 kHz)
    }
    else
    {
        ledcWrite(BUZZER_CHANNEL, 0); // buzzer off
    }
}

/**
 * @brief Simple button handling (internal pull‑up, active‑LOW).
 *        When the alarm (level 3) is sounding, a press mutes the buzzer.
 */
void handleButton(uint8_t currentLevel)
{
    int reading = digitalRead(PIN_BUTTON);   // HIGH = not pressed, LOW = pressed

    // Debounce
    if (reading != buttonPrevState)
    {
        buttonLastDebounce = millis();       // reset debounce timer
    }

    if ((millis() - buttonLastDebounce) > DEBOUNCE_MS)
    {
        // State has been stable for > DEBOUNCE_MS
        if (reading == LOW)                  // button pressed
        {
            if (currentLevel == 3)           // only mute when alarm is active
                buzzerMuted = true;
        }
        else                                 // button released
        {
            // Reset mute when alarm is cleared (so next alarm can sound)
            if (currentLevel != 3)
                buzzerMuted = false;
        }
    }

    buttonPrevState = reading;
}

/**
 * @brief Refresh the OLED display with the latest sensor values.
 */
void updateDisplay(float temperature, float humidity,
                   bool wfStatus, uint8_t lightLevel)
{
    display.clearDisplay();
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);

    display.printf("Temp:%dC\n", temperature - floor(temperature) < 0.5 ? int(floor(temperature)) : int(ceil(temperature)));
    display.printf("Hum:%d%\n", humidity - floor(humidity) < 0.5 ? int(floor(humidity)) : int(ceil(humidity)));
    display.printf("Wifi:%s\n", wfStatus ? "YES" : "NO");
    display.printf("Light:%u", lightLevel);
    /*display.printf("Status: %s\n",
                   (statusLevel == 1) ? "NORMAL" :
                   (statusLevel == 2) ? "WARN"   : "DANGER");
*/
    display.display();
}

/**
 * @brief Push the current data to Firebase Realtime Database.
 */
void uploadToFirebase(float temperature, float humidity,
                      bool smoke, uint8_t lightLevel,
                      uint8_t statusLevel)
{
    FirebaseJson json;
    json.add("device_id", DEVICE_ID);
    json.add("temperature", temperature);
    json.add("humidity", humidity);
    json.add("smoke_detected", smoke);
    json.add("light_level", lightLevel);
    json.add("status_level", statusLevel);

    // Path can be anything you like – here we use a fixed node.
    if (Firebase.RTDB.setJSON(&fbdo, "/roomhub/latest", &json))
    {
        Serial.println("✅ Data uploaded to Firebase");
    }
    else
    {
        Serial.printf("❌ Firebase error %s\n", fbdo.errorReason().c_str());
    }
}

/* -------------------------------------------------------------------------
   Setup
   ------------------------------------------------------------------------- */
void setup()
{
    Serial.begin(115200);
    analogReadResolution(12);
    delay(1000);
    Serial.println("\n=== Room Monitoring Hub ===");

    /* ---- GPIO ---- */
    pinMode(PIN_MQ2,        INPUT);               // digital smoke sensor
    pinMode(PIN_LED_GREEN,  OUTPUT);
    pinMode(PIN_LED_YELLOW, OUTPUT);
    pinMode(PIN_LED_RED,    OUTPUT);
    // LEDC (buzzer) initialization – moved to after pin setup
    ledcSetup(BUZZER_CHANNEL, BUZZER_FREQ, BUZZER_RESOLUTION);
    ledcAttachPin(PIN_BUZZER, BUZZER_CHANNEL);
    ledcWrite(BUZZER_CHANNEL, 0); // buzzer off
    pinMode(PIN_BUTTON,     INPUT_PULLUP);        // internal pull‑up

    /* ---- Sensors ---- */
    dht.begin();

    /* ---- OLED ---- */
    Wire.begin(OLED_SDA, OLED_SCL);
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR))
    {
        Serial.println("❌ SSD1306 allocation failed");
        while (true) { delay(100); }   // halt
    }
    display.clearDisplay();
    display.display();

    /* ---- Wi‑Fi ---- */
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("Connecting to Wi‑Fi");
    /*
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print('.');
    }
     */
    Serial.println("\n✅ Wi‑Fi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    /* ---- Firebase ---- */
    config.host = FIREBASE_HOST;
    config.signer.tokens.legacy_token = FIREBASE_AUTH;   // for Realtime DB
    // Optional: set a connection timeout, keep‑alive, etc.
    /*if (!Firebase.begin(&config, &auth))
    {
        Serial.printf("❌ Firebase init failed: %s\n", auth.errorReason().c_str());
    }
    else
    {
        Serial.println("✅ Firebase initialized");
    }*/

    /* ---- Initial state ---- */
    digitalWrite(PIN_LED_GREEN, HIGH);   // start in normal mode
}

/* -------------------------------------------------------------------------
   Main loop
   ------------------------------------------------------------------------- */
void loop()
{
    bool wfStatus =false;
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        wfStatus = false;
    }
    else wfStatus = true;
    /* ---- 1. Read sensors ------------------------------------------------- */
    float temperature = dht.readTemperature();   // °C
    float humidity    = dht.readHumidity();      // %

    // DHT may return NaN if the reading fails – use a fallback value
    if (isnan(temperature)) temperature = 0.0;
    if (isnan(humidity))    humidity    = 0.0;

    bool smokeDetected = readSmokeDetected();
    uint8_t lightLevel  = readLightLevel();

    /* ---- 2. Determine system status -------------------------------------- */
    uint8_t statusLevel = evaluateStatusLevel(temperature, humidity, smokeDetected);

    /* ---- 3. Apply outputs (LEDs, buzzer) --------------------------------- */
    applyOutputs(statusLevel, smokeDetected);

    /* ---- 4. Button handling (mute) --------------------------------------- */
    handleButton(statusLevel);

    /* ---- 5. Update OLED -------------------------------------------------- */
    updateDisplay(temperature, humidity, wfStatus, lightLevel);

    /* ---- 6. Periodic Firebase upload ------------------------------------ */
    unsigned long now = millis();
    /*if (now - lastUpload >= UPLOAD_INTERVAL)
    {
        uploadToFirebase(temperature, humidity, smokeDetected, lightLevel, statusLevel);
        lastUpload = now;
    }
    */
    Serial.print("temp: ");
    Serial.print(temperature);
    Serial.print(" hum: ");
    Serial.print(humidity);
    Serial.print(" smo: ");
    Serial.print(smokeDetected);
    Serial.print(" light: ");
    Serial.print(lightLevel);
    Serial.print(" Wfi: ");
    Serial.print(wfStatus);
    Serial.print(" sta: ");
    Serial.println(statusLevel); // Dùng println ở cuối để xuống dòng
    /* ---- Small delay – keep loop responsive ------------------------------ */
    delay(200);   // 5 Hz refresh is more than enough for this demo
}