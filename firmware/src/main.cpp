/*
Room Monitoring Hub

• DHT22  – GPIO4
• MQ‑2   – GPIO5
• LDR    – GPIO34
• OLED   – SDA‑21, SCL‑22
• LEDs   – Green-12, Yellow-13, Red-14
• Buzzer – 27
• Button – pull‑up (26)
 
    Author : Nguyễn Lê Phước Vinh
*/

#include <Arduino.h>
#include <WiFi.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>
#include <dns.h>

/* ---------------------------  Pin define  --------------------------- */
#define PIN_DHT          4         
#define PIN_MQ2          5          
#define PIN_LDR          34         
#define PIN_LED_GREEN    12
#define PIN_LED_YELLOW   13
#define PIN_LED_RED      14
#define PIN_BUZZER       27
#define PIN_BUTTON       26         //pull‑up

/* ---------------------------  OLED   ---------------------------- */
#define OLED_SDA         21
#define OLED_SCL         22
#define OLED_ADDR        0x3C       
#define SCREEN_WIDTH     128
#define SCREEN_HEIGHT    64
#define OLED_RESET      -1          

/* ---------------------------  Const -------------------------- */
#define DEVICE_ID        "ROOMHUB00001"
#define UPLOAD_INTERVAL  5000UL      

float TEMP_WARNING = 30.0;
float TEMP_DANGER  = 37.0;
float HUM_WARNING  = 70.0;
float HUM_DANGER   = 80.0;

/* ----------  Buzzer ---------- */
#define BUZZER_CHANNEL   0          
#define BUZZER_FREQ      2000       // 2 kHz tone frequency
#define BUZZER_RESOLUTION 8         // 8‑bit (0‑255)

/* ---------------------------  Global  --------------------------- */
DHT dht(PIN_DHT, DHT22);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

/* ---------------------------  Variables  ------------------------ */
unsigned long lastUpload = 0;
bool buzzerMuted = false;          
bool buttonPrevState = HIGH;      
unsigned long buttonLastDebounce = 0;
const unsigned long DEBOUNCE_MS = 50;

uint8_t readLightLevel()
{
    int raw = analogRead(PIN_LDR); 
    
    // 0-4095 to 0-10
    uint8_t level = map(raw, 0, 4095, 0, 10);
    
    return constrain(level, 0, 10);
}

bool readSmokeDetected()
{
    int state = digitalRead(PIN_MQ2);
    return (state == LOW);
}

uint8_t evaluateStatusLevel(float temperature, float humidity, bool smoke)
{
    if (smoke) return 3;                                 // smoke 

    if (temperature >= TEMP_DANGER || humidity >= HUM_DANGER)
        return 3;                                         // danger

    if (temperature >= TEMP_WARNING || humidity >= HUM_WARNING)
        return 2;                                         // warning

    return 1;                                            // normal
}

void applyOutputs(uint8_t level, bool smoke)
{
    // LEDs 
    digitalWrite(PIN_LED_GREEN,  (level == 1) ? HIGH : LOW);
    digitalWrite(PIN_LED_YELLOW, (level == 2) ? HIGH : LOW);
    digitalWrite(PIN_LED_RED,    (level == 3) ? HIGH : LOW);

    // Buzzer
    if (level == 3 && !buzzerMuted)
    {
        ledcWrite(BUZZER_CHANNEL, 128); // 2 kHz
    }
    else
    {
        ledcWrite(BUZZER_CHANNEL, 0); // off
    }
}

void handleButton(uint8_t currentLevel)
{
    int reading = digitalRead(PIN_BUTTON);   // HIGH not pressed, LOW pressed

    if (reading != buttonPrevState)
    {
        buttonLastDebounce = millis();       
    }

    if ((millis() - buttonLastDebounce) > DEBOUNCE_MS)
    {
        if (reading == LOW)                  // button pressed
        {
            if (currentLevel == 3)           // only mute when alarm is active
                buzzerMuted = true;
        }
        else                                 // button released
        {
            if (currentLevel != 3)
                buzzerMuted = false;
        }
    }

    buttonPrevState = reading;
}

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

    display.display();
}

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

    if (Firebase.RTDB.setJSON(&fbdo, "/roomhub/latest", &json))
    {
        Serial.println("✅ Data uploaded to Firebase");
    }
    else
    {
        Serial.printf("❌ Firebase error %s\n", fbdo.errorReason().c_str());
    }
}
void readSettingsFromFirebase() 
{
    // temp_warning
    if (Firebase.RTDB.getFloat(&fbdo, "/roomhub/settings/temp_warning")) {
        TEMP_WARNING = fbdo.floatData();
    }
    
    if (Firebase.RTDB.getFloat(&fbdo, "/roomhub/settings/temp_danger")) {
        TEMP_DANGER = fbdo.floatData();
    }
    // hum_warning
    if (Firebase.RTDB.getFloat(&fbdo, "/roomhub/settings/hum_warning")) {
        HUM_WARNING = fbdo.floatData();
    }
    
    if (Firebase.RTDB.getFloat(&fbdo, "/roomhub/settings/hum_danger")) {
        HUM_DANGER = fbdo.floatData();
    }
}

void setup()
{
    Serial.begin(115200);
    analogReadResolution(12);
    delay(1000);
    Serial.println("\n=== Room Monitoring Hub ===");

    // GPIO
    pinMode(PIN_MQ2,        INPUT);               
    pinMode(PIN_LED_GREEN,  OUTPUT);
    pinMode(PIN_LED_YELLOW, OUTPUT);
    pinMode(PIN_LED_RED,    OUTPUT);
    // Buzzer
    ledcSetup(BUZZER_CHANNEL, BUZZER_FREQ, BUZZER_RESOLUTION);
    ledcAttachPin(PIN_BUZZER, BUZZER_CHANNEL);
    ledcWrite(BUZZER_CHANNEL, 0); // buzzer off
    pinMode(PIN_BUTTON,     INPUT_PULLUP);        // pull‑up

    // Sensors
    dht.begin();

    // OLED
    Wire.begin(OLED_SDA, OLED_SCL);
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR))
    {
        Serial.println("❌ SSD1306 allocation failed");
        while (true) { delay(100); }   
    }
    display.clearDisplay();
    display.display();

    // Wi‑Fi 
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    WiFi.setAutoReconnect(true);
    Serial.print("Connecting to Wi‑Fi");
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 20)
    {
        delay(500);
        Serial.print('.');
        retry++;
    }
    if (WiFi.status() == WL_CONNECTED){
        Serial.println("\n✅ Wi‑Fi connected");
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());

        // Firebase
        config.api_key = API_KEY;
        config.database_url = DATABASE_URL;

        config.token_status_callback = tokenStatusCallback;
        config.signer.test_mode = true;

        if (Firebase.signUp(&config, &auth, "", "")) {
            Serial.println("✅ Firebase Authenticated");
        } else {
            Serial.printf("❌ Firebase error: %s\n", config.signer.signupError.message.c_str());
        }
        if (Firebase.ready()){
            Firebase.begin(&config, &auth);
            Firebase.reconnectWiFi(true);
        }
    }
    else Serial.println("No Wifi connected, Offline mode");
    // Initial state
    digitalWrite(PIN_LED_GREEN, HIGH);   
}

void loop()
{
    bool wfStatus = (WiFi.status() == WL_CONNECTED);
    // Read sensors
    float temperature = dht.readTemperature();   
    float humidity    = dht.readHumidity();      

    if (isnan(temperature)) temperature = 0.0;
    if (isnan(humidity))    humidity    = 0.0;

    bool smokeDetected = readSmokeDetected();
    uint8_t lightLevel  = readLightLevel();

    // System status
    uint8_t statusLevel = evaluateStatusLevel(temperature, humidity, smokeDetected);

    applyOutputs(statusLevel, smokeDetected);

    // Button
    handleButton(statusLevel);

    // OLED
    updateDisplay(temperature, humidity, wfStatus, lightLevel);

    unsigned long now = millis();
    if (now - lastUpload >= UPLOAD_INTERVAL && Firebase.ready())
    {
        uploadToFirebase(temperature, humidity, smokeDetected, lightLevel, statusLevel);

        readSettingsFromFirebase();
        
        lastUpload = now;
    }
    
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
    Serial.println(statusLevel); 
    delay(200);   
}