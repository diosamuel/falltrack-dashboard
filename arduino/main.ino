#include <TinyGPSPlus.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include "AudioFileSourceSPIFFS.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

// Task handles for dual core operation
TaskHandle_t audioTask;
TaskHandle_t mainTask;

// Mutex for shared resource protection
portMUX_TYPE mutex = portMUX_INITIALIZER_UNLOCKED;

// GYRO ACCELEROMETER
Adafruit_MPU6050 mpu;

// WiFi credentials
const char *ssid = "router zte link";
const char *password = "batuponari";

// GPS
TinyGPSPlus gps;
HardwareSerial GPSserial(2);
#define RXD2 16
#define TXD2 17

// Buzzer
#define BUZZER_PIN 4

// Navigation Button
#define NAVIGATION_BUTTON_PIN 0  // G0 pin
unsigned long lastButtonPress = 0;  // Debounce timer
const unsigned long DEBOUNCE_DELAY = 250;  // Debounce delay in milliseconds

// Origin
double originLat = 0.0;
double originLng = 0.0;
bool hasOrigin = false;
const double DISTANCE_THRESHOLD = 2.0;

const char *HOST_DOMAIN = "https://fallertrack-be.my.id";
// // HTTPS endpoint POST/GET
// const char *GPS_DISTANCE_API = "https://fallertrack.my.id/api/current-distance";
// const char *GYRO_FALL_API = "https://fallertrack.my.id/api/fall-detection";
// const char *NAVIGATION_API = "https://fallertrack.my.id/api/text-to-speech";

// // HTTPS endpoint GET
// const char *ALERT_SOS_API = "https://fallertrack.my.id/api/alert";

// Base API domain
// const char* HOST_DOMAIN = "https://fallertrack.my.id";

// HTTPS endpoints as String
String GPS_DISTANCE_API = String(HOST_DOMAIN) + "/api/current-distance";
String GYRO_FALL_API     = String(HOST_DOMAIN) + "/api/fall-detection";
String NAVIGATION_API    = String(HOST_DOMAIN) + "/api/text-to-speech";
String ALERT_SOS_API     = String(HOST_DOMAIN) + "/api/alert";


unsigned long lastPostTime = 0;
const unsigned long postInterval = 3000; // 3 seconds

// Audio components
AudioGeneratorMP3* mp3 = nullptr;
AudioFileSourceSPIFFS* file = nullptr;
AudioOutputI2S* out = nullptr;
bool shouldPlayAudio = false;
String pendingAudioUrl = "";

// Add these global variables after other declarations
unsigned long lastWiFiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL = 5000; // Check WiFi every 5 seconds
bool isWiFiConnected = false;

// Function to download and play MP3
void downloadAndPlayMP3(const char* url) {
    portENTER_CRITICAL(&mutex);
    pendingAudioUrl = String(url);
    shouldPlayAudio = true;
    portEXIT_CRITICAL(&mutex);
}

// Audio Task - Runs on Core 0
void audioTaskFunction(void * parameter) {
    for(;;) {
        if (shouldPlayAudio) {
            portENTER_CRITICAL(&mutex);
            String currentUrl = pendingAudioUrl;
            shouldPlayAudio = false;
            portEXIT_CRITICAL(&mutex);

            // Clean up previous audio if any
            if (mp3) {
                mp3->stop();
                delete file;
                delete out;
                delete mp3;
                mp3 = nullptr;
            }

            WiFiClientSecure client;
            client.setInsecure();

            String host = currentUrl.substring(currentUrl.indexOf("//") + 2, currentUrl.indexOf("/", 8));
            String path = currentUrl.substring(currentUrl.indexOf("/", 8));

            if (client.connect(host.c_str(), 443)) {
                client.print(String("GET ") + path + " HTTP/1.1\r\n" +
                           "Host: " + host + "\r\n" +
                           "Connection: close\r\n\r\n");

                // Skip headers
                while (client.connected()) {
                    String line = client.readStringUntil('\n');
                    if (line == "\r") break;
                }

                // Download and play file
                File f = SPIFFS.open("/nav.mp3", FILE_WRITE);
                if (f) {
                    while (client.connected() || client.available()) {
                        uint8_t buf[512];
                        int len = client.read(buf, sizeof(buf));
                        if (len > 0) {
                            f.write(buf, len);
                        } else {
                            delay(1);
                        }
                    }
                    f.close();

                    // Play the audio
                    file = new AudioFileSourceSPIFFS("/nav.mp3");
                    out = new AudioOutputI2S(0, AudioOutputI2S::INTERNAL_DAC);
                    out->SetOutputModeMono(true);
                    out->SetGain(0.5);

                    mp3 = new AudioGeneratorMP3();
                    mp3->begin(file, out);

                    // Play until complete
                    while (mp3->isRunning()) {
                        if (!mp3->loop()) {
                            mp3->stop();
                        }
                        vTaskDelay(1); // Allow other tasks to run
                    }
                }
            }
        }
        vTaskDelay(1); // Prevent watchdog trigger
    }
}

// Main Task - Runs on Core 1
void mainTaskFunction(void * parameter) {
    for(;;) {
        // Check WiFi connection status periodically
        if (millis() - lastWiFiCheck >= WIFI_CHECK_INTERVAL) {
            lastWiFiCheck = millis();
            if (WiFi.status() != WL_CONNECTED) {
                Serial.println("WiFi disconnected! Reconnecting...");
                WiFi.reconnect();
                delay(500);
                isWiFiConnected = false;
            } else if (!isWiFiConnected) {
                Serial.println("WiFi reconnected!");
                isWiFiConnected = true;
            }
        }

        // Check for alerts only if WiFi is connected
        if (isWiFiConnected) {
            checkAlertStatus();
        }

        // Check navigation button with debounce
        if (digitalRead(NAVIGATION_BUTTON_PIN) == LOW) {
            unsigned long currentTime = millis();
            if (currentTime - lastButtonPress > DEBOUNCE_DELAY) {
                Serial.println("Navigation button pressed!");
                if (isWiFiConnected) {
                    getNavigationInstructions();
                } else {
                    Serial.println("Cannot get navigation - WiFi not connected");
                }
                lastButtonPress = currentTime;
            }
        }

        sensors_event_t a, g, temp;
        mpu.getEvent(&a, &g, &temp);

        while (GPSserial.available()) {
            gps.encode(GPSserial.read());
        }

        // Enhanced GPS validation
        if (gps.location.isValid() && gps.satellites.value() >= 3 && gps.hdop.value() < 300) {
            double currentLat = gps.location.lat();
            double currentLng = gps.location.lng();

            if (!hasOrigin) {
                originLat = currentLat;
                originLng = currentLng;
                hasOrigin = true;
                Serial.println("Origin location set.");
            } else {
                if (millis() - lastPostTime >= postInterval) {
                    lastPostTime = millis();
                    
                    if (isWiFiConnected) {
                        // Print debug info
                        Serial.println("Posting GPS Data:");
                        Serial.print("Lat: "); Serial.print(currentLat, 6);
                        Serial.print(" Lng: "); Serial.println(currentLng, 6);
                        Serial.print("Satellites: "); Serial.println(gps.satellites.value());
                        Serial.print("HDOP: "); Serial.println(gps.hdop.value());
                        
                        postGPSData(currentLat, currentLng);
                        
                        float acceleroData[3] = {
                            a.acceleration.x,
                            a.acceleration.y,
                            a.acceleration.z
                        };
                        float gyroData[3] = {
                            g.gyro.x,
                            g.gyro.y,
                            g.gyro.z
                        };
                        
                        // Print debug info
                        Serial.println("Posting Accelerometer Data:");
                        Serial.printf("Accelero: X=%.2f Y=%.2f Z=%.2f\n", 
                            acceleroData[0], acceleroData[1], acceleroData[2]);
                        Serial.printf("Gyro: X=%.2f Y=%.2f Z=%.2f\n", 
                            gyroData[0], gyroData[1], gyroData[2]);
                            
                        postAccelerometerData(acceleroData, gyroData);
                    } else {
                        Serial.println("Cannot post data - WiFi not connected");
                    }
                }
            }
        } else {
            if (gps.location.isValid()) {
                Serial.println("GPS signal too weak or inaccurate:");
                Serial.print("Satellites: "); Serial.println(gps.satellites.value());
                Serial.print("HDOP: "); Serial.println(gps.hdop.value());
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100)); // 100ms delay
    }
}

void setup() {
    Serial.begin(115200);
    Wire.begin(22, 21);

    // Initialize pins
    pinMode(NAVIGATION_BUTTON_PIN, INPUT_PULLUP);
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    // Init GPS
    GPSserial.begin(9600, SERIAL_8N1, RXD2, TXD2);

    // Init SPIFFS
    if (!SPIFFS.begin(true)) {
        Serial.println("SPIFFS initialization failed!");
        return;
    }

    // Init WiFi
    Serial.println("Connecting to WiFi...");
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi Connected!");

    // Init MPU6050
    if (mpu.begin()) {
        Serial.println("Failed to find MPU6050 chip");
        while (1) {
            delay(10);
        }
    }
    Serial.println("MPU6050 Found!");

    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_5_HZ);

    // Create tasks for both cores
    xTaskCreatePinnedToCore(
        audioTaskFunction,    // Function to implement the task
        "audioTask",         // Name of the task
        10000,              // Stack size in words
        NULL,               // Task input parameter
        1,                  // Priority of the task
        &audioTask,         // Task handle
        0                   // Core where the task should run
    );

    xTaskCreatePinnedToCore(
        mainTaskFunction,    // Function to implement the task
        "mainTask",         // Name of the task
        10000,              // Stack size in words
        NULL,               // Task input parameter
        1,                  // Priority of the task
        &mainTask,          // Task handle
        1                   // Core where the task should run
    );
}

void loop() {
    // Empty loop - tasks are handling everything
    vTaskDelay(portMAX_DELAY);
}

// Function to get navigation instructions
void getNavigationInstructions() {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;

    if (https.begin(client, NAVIGATION_API)) {
        int httpResponseCode = https.GET();
        Serial.print("Navigation GET Response Code: ");
        Serial.println(httpResponseCode);
        
        if (httpResponseCode > 0) {
            String response = https.getString();
            Serial.println("Navigation Instructions: " + response);

            // Parse JSON response
            StaticJsonDocument<1024> doc;
            DeserializationError error = deserializeJson(doc, response);

            if (!error) {
                const char* text = doc["text"];
                const char* url = doc["bucketStatus"]["url"];
                Serial.println("Navigation text: " + String(text));
                Serial.println("Audio URL: " + String(url));

                // Download and play the audio if URL is available
                if (url) {
                    downloadAndPlayMP3(url);
                }
            }
        }
        
        https.end();
    } else {
        Serial.println("Navigation HTTPS connection failed");
    }
}

// Function to post GPS data
void postGPSData(double latitude, double longitude) {
                WiFiClientSecure client;
                client.setInsecure(); // Not validating SSL certificate (for testing only)
                HTTPClient https;

    if (https.begin(client, GPS_DISTANCE_API)) {
                    https.addHeader("Content-Type", "application/json");

                    String jsonData = "{\"latitude\":";
        jsonData += String(latitude, 6); // Increase precision to 6 decimal places
                    jsonData += ",\"longitude\":";
        jsonData += String(longitude, 6);
                    jsonData += "}";

                    int httpResponseCode = https.POST(jsonData);
        Serial.print("GPS POST Response Code: ");
                    Serial.println(httpResponseCode);
        
        if (httpResponseCode > 0) {
            String response = https.getString();
            Serial.println("GPS Response: " + response);
        } else {
            Serial.print("GPS POST Error: ");
            Serial.println(https.errorToString(httpResponseCode));
        }
        
                    https.end();
    } else {
        Serial.println("GPS HTTPS connection failed");
    }
}

// Function to post accelerometer data
void postAccelerometerData(float accelero[3], float gyro[3]) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;

    if (https.begin(client, GYRO_FALL_API)) {
                    https.addHeader("Content-Type", "application/json");

        String jsonData = "{\"accelero\":[";
        jsonData += String(accelero[0], 2) + ",";  // Increase precision to 2 decimal places
        jsonData += String(accelero[1], 2) + ",";
        jsonData += String(accelero[2], 2);
        jsonData += "],\"gyro\":[";
        jsonData += String(gyro[0], 2) + ",";
        jsonData += String(gyro[1], 2) + ",";
        jsonData += String(gyro[2], 2);
        jsonData += "]}";

                    int httpResponseCode = https.POST(jsonData);
        Serial.print("Accelerometer POST Response Code: ");
                    Serial.println(httpResponseCode);
        
        if (httpResponseCode > 0) {
            String response = https.getString();
            Serial.println("Accelerometer Response: " + response);
        } else {
            Serial.print("Accelerometer POST Error: ");
            Serial.println(https.errorToString(httpResponseCode));
        }
        
                    https.end();
    } else {
        Serial.println("Accelerometer HTTPS connection failed");
    }
}

// Function to check alert status
void checkAlertStatus() {
    WiFiClientSecure client;
    client.setInsecure(); // Not validating SSL certificate (for testing only)
    HTTPClient https;

    if (https.begin(client, ALERT_SOS_API)) {
        int httpResponseCode = https.GET();
        Serial.print("Alert GET Response Code: ");
        Serial.println(httpResponseCode);
        
        if (httpResponseCode > 0) {
            String response = https.getString();
            Serial.println("Alert Response: " + response);
            
            // Parse JSON response
            // Parse JSON response
            StaticJsonDocument<200> doc;
            DeserializationError error = deserializeJson(doc, response);
            
            if (!error && doc["sos"].as<bool>()) {
                // Alert is active, trigger buzzer
                digitalWrite(BUZZER_PIN, HIGH);
                delay(1000);  // Beep for 500ms
                digitalWrite(BUZZER_PIN, LOW); 
                delay(100);  // Pause for 500ms
            } else {
                // No alert, ensure buzzer is off
                digitalWrite(BUZZER_PIN, LOW);
            }
        }
        
        https.end();
    } else {
        Serial.println("Alert HTTPS connection failed");
    }
}