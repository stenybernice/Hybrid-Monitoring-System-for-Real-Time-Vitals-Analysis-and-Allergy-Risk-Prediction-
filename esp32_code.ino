// ============================================================
// IoT Vitals Monitor - ESP32 PRODUCTION-STABLE v2
// Sensors: MAX30102 (HR + SpO2), MLX90614 (IR Temp)
//
// FIX in this version:
// esp_task_wdt_init() now uses esp_task_wdt_config_t struct
// required by ESP32 Arduino core v3.x (ESP-IDF 5.x)
// Old 2-arg form: esp_task_wdt_init(10, true) ← ERROR
// New struct form: esp_task_wdt_init(&wdt_config) ← CORRECT
// ============================================================

#include <Wire.h>
#include <math.h>
#include "MAX30105.h"
#include "spo2_algorithm.h"
#include <Adafruit_MLX90614.h>
#include "esp_task_wdt.h"

// ---------- Sensors ----------
MAX30105 particleSensor;
Adafruit_MLX90614 mlx = Adafruit_MLX90614();

// ---------- Model Params ----------
const float SCALER_MEAN[3]  = {75.0f, 97.5f, 36.1f};
const float SCALER_SCALE[3] = {12.0f, 1.5f, 0.8f};

const float CENTROIDS[3][3] = {
  { 0.10f, -0.25f, 0.05f},
  { 1.20f,  0.30f, 2.10f},
  { 2.50f, -8.00f, -0.30f}
};

const int   HEALTHY_CLUSTER    = 0;
const float THRESHOLD_WARNING  = 3.0f;
const float THRESHOLD_CRITICAL = 5.0f;

// ---------- MAX30102 Buffers ----------
#define BUFFER_LENGTH 100
#define NEW_SAMPLES   25

uint32_t irBuffer[BUFFER_LENGTH];
uint32_t redBuffer[BUFFER_LENGTH];

int32_t spo2Value      = 0;
int8_t  validSPO2      = 0;
int32_t heartRate      = 0;
int8_t  validHeartRate = 0;

// ---------- State ----------
float    tempFiltered = 36.0f;
bool     tempReady    = false;
uint32_t lastTempMs   = 0;
uint32_t lastOutputMs = 0;
uint32_t lastSpO2Ms   = 0;
// ================== NEW: LED + BUZZER + STREAK ==================
#define RED_LED_PIN    25
#define GREEN_LED_PIN  4
#define BUZZER_PIN     27

int streakCounter = 0;
const int STREAK_THRESHOLD = 25;

// previous values for trend detection
float prevHR = 0;
float prevSpO2 = 0;
// ============================================================

// ============================================================
// WDT helper — works on both core v2 and v3
// ============================================================

// =================================================
void initWatchdog() {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  // Core v3.x / IDF 5.x — must use config struct
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms     = 10000, // 10 second timeout
    .idle_core_mask = 0,     // don't watch idle tasks
    .trigger_panic  = true   // reset on timeout
  };
  esp_task_wdt_init(&wdt_config);
#else
  // Core v2.x / IDF 4.x — old 2-arg API
  esp_task_wdt_init(10, true);
#endif
  esp_task_wdt_add(NULL); // subscribe current task
}

// ============================================================
// standardize
// ============================================================
void standardize(float hr, float spo2, float temp, float scaled[3]) {
  float raw[3] = {hr, spo2, temp};
  for (int i = 0; i < 3; i++)
    scaled[i] = (raw[i] - SCALER_MEAN[i]) / SCALER_SCALE[i];
}

// ============================================================
// predictCluster
// ============================================================
int predictCluster(float scaled[3]) {
  int best = 0;
  float bestDist = 1e9f;

  for (int k = 0; k < 3; k++) {
    float dist = 0;

    for (int j = 0; j < 3; j++) {
      float d = scaled[j] - CENTROIDS[k][j];
      dist += d * d;
    }

    dist = sqrtf(dist);

    if (dist < bestDist) {
      bestDist = dist;
      best = k;
    }
  }
  return best;
}

// ============================================================
// computeRisk
// ============================================================
float computeRisk(float scaled[3]) {
  float dist = 0;

  for (int i = 0; i < 3; i++) {
    float d = scaled[i] - CENTROIDS[HEALTHY_CLUSTER][i];
    dist += d * d;
  }
  return sqrtf(dist);
}

const char* severity(float r) {
  if (r > THRESHOLD_CRITICAL) return "CRITICAL";
  if (r > THRESHOLD_WARNING)  return "WARNING";
  return "SAFE";
}

// ============================================================
// safeReadTemp — never returns NaN
// ============================================================
float safeReadTemp() {
  float t = mlx.readObjectTempC();
  if (isnan(t) || t < 20.0f || t > 50.0f) return tempFiltered;
  return t;
}

// ============================================================
// fillBuffer — WDT-safe initial fill
// ============================================================
void fillBuffer() {
  Serial.println("Filling sample buffer...");

  int count = 0;
  uint32_t startMs = millis();

  while (count < BUFFER_LENGTH) {
    esp_task_wdt_reset();
    particleSensor.check();

    while (particleSensor.available() && count < BUFFER_LENGTH) {
      redBuffer[count] = particleSensor.getRed();
      irBuffer[count]  = particleSensor.getIR();
      particleSensor.nextSample();
      count++;
    }

    if (millis() - startMs > 10000) {
      Serial.println("WARN: buffer fill timeout, padding remainder.");
      while (count < BUFFER_LENGTH) {
        int prev = count > 0 ? count - 1 : 0;
        redBuffer[count] = redBuffer[prev];
        irBuffer[count]  = irBuffer[prev];
        count++;
      }
      break;
    }

    delay(2);
  }

  Serial.println("Buffer ready.");
}

// ============================================================
// setup
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(300);

  initWatchdog(); // uses correct API for installed core version

  Wire.begin(21, 22);
  Wire.setClock(40000);
  delay(200);

  Serial.println("Booting system...");

  // ================= NEW: pin setup =================
pinMode(RED_LED_PIN, OUTPUT);
pinMode(GREEN_LED_PIN, OUTPUT);
pinMode(BUZZER_PIN, OUTPUT);

digitalWrite(RED_LED_PIN, LOW);
digitalWrite(GREEN_LED_PIN, LOW);
digitalWrite(BUZZER_PIN, LOW);
// =================================================
  
  // ---- MAX30102 ----
  if (!particleSensor.begin(Wire, I2C_SPEED_STANDARD)) {
    Serial.println("ERROR: MAX30102 not found. Check wiring.");
    while (true) { esp_task_wdt_reset(); delay(500); }
  }

  particleSensor.softReset();
  delay(100);
  particleSensor.setup(60, 4, 2, 100, 411, 4096);
  particleSensor.check();

  // ---- MLX90614 ----
  if (!mlx.begin()) {
    Serial.println("ERROR: MLX90614 not found. Check wiring.");
    while (true) { esp_task_wdt_reset(); delay(500); }
  }

  // Warm-up reads
  for (int i = 0; i < 15; i++) {
    esp_task_wdt_reset();
    float t = mlx.readObjectTempC();
    if (!isnan(t) && t > 20.0f && t < 50.0f) tempFiltered = t;
    delay(100);
  }

  tempReady = true;

  fillBuffer();

  maxim_heart_rate_and_oxygen_saturation(
    irBuffer, BUFFER_LENGTH, redBuffer,
    &spo2Value, &validSPO2, &heartRate, &validHeartRate
  );

  Serial.println("Sensors ready. Streaming...");
}

// ============================================================
// loop — fully non-blocking, WDT-safe
// ============================================================
void loop() {
  esp_task_wdt_reset();

  uint32_t now = millis();

  // Temperature — every 500 ms
  if (now - lastTempMs >= 500) {
    lastTempMs = now;
    float t = safeReadTemp();
    tempFiltered = 0.7f * tempFiltered + 0.3f * t;
  }

  // SpO2 / HR — every 1000 ms
  if (now - lastSpO2Ms >= 1000) {
    lastSpO2Ms = now;

    // Shift old samples down
    for (int i = 0; i < (BUFFER_LENGTH - NEW_SAMPLES); i++) {
      redBuffer[i] = redBuffer[i + NEW_SAMPLES];
      irBuffer[i]  = irBuffer[i + NEW_SAMPLES];
    }

    // Collect NEW_SAMPLES fresh readings
    int collected = 0;
    uint32_t startMs = millis();

    while (collected < NEW_SAMPLES) {
      esp_task_wdt_reset();
      particleSensor.check();

      while (particleSensor.available() && collected < NEW_SAMPLES) {
        int idx = (BUFFER_LENGTH - NEW_SAMPLES) + collected;
        redBuffer[idx] = particleSensor.getRed();
        irBuffer[idx]  = particleSensor.getIR();
        particleSensor.nextSample();
        collected++;
      }

      if (millis() - startMs > 800) {
        while (collected < NEW_SAMPLES) {
          int idx = (BUFFER_LENGTH - NEW_SAMPLES) + collected;
          int prev = idx > 0 ? idx - 1 : 0;
          redBuffer[idx] = redBuffer[prev];
          irBuffer[idx]  = irBuffer[prev];
          collected++;
        }
        break;
      }

      delay(1);
    }

    maxim_heart_rate_and_oxygen_saturation(
      irBuffer, BUFFER_LENGTH, redBuffer,
      &spo2Value, &validSPO2, &heartRate, &validHeartRate
    );
  }

  // Serial output — every 1500 ms
  if (now - lastOutputMs >= 1500) {
    lastOutputMs = now;

    if (!validHeartRate || !validSPO2) {
      Serial.println("Waiting for valid signal...");
      return;
    }

    float hr   = (float)heartRate;
    float spo2 = (float)spo2Value;
    float temp = tempFiltered;

    if (hr < 40 || hr > 160 || spo2 < 80 || temp < 30 || temp > 45) {
      Serial.printf("Out-of-range: HR=%.0f SpO2=%.0f Temp=%.2fC\n",
                    hr, spo2, temp);
      return;
    }

    float scaled[3];
    standardize(hr, spo2, temp, scaled);

    int cluster = predictCluster(scaled);
    float risk  = computeRisk(scaled);
    // ================= NEW: streak + LED + buzzer =================

// trend detection
bool dropping = (hr < prevHR) && (spo2 < prevSpO2);

if (dropping) {
  streakCounter++;
} else {
  streakCounter = 0;
}

prevHR = hr;
prevSpO2 = spo2;

// -------- LED LOGIC --------
// SAFE + CRITICAL → GREEN
// WARNING → RED

if (risk > THRESHOLD_WARNING && risk <= THRESHOLD_CRITICAL) {
  digitalWrite(RED_LED_PIN, HIGH);
  digitalWrite(GREEN_LED_PIN, LOW);
} else {
  digitalWrite(RED_LED_PIN, LOW);
  digitalWrite(GREEN_LED_PIN, HIGH);
}

// -------- BUZZER --------
// WARNING streak > 25

if ((risk > THRESHOLD_WARNING && risk <= THRESHOLD_CRITICAL) && streakCounter > STREAK_THRESHOLD) {
  digitalWrite(BUZZER_PIN, HIGH);
} else {
  digitalWrite(BUZZER_PIN, LOW);
}

// ============================================================

    Serial.println("------ VITALS ------");
    Serial.printf("HR: %.0f bpm | SpO2: %.0f%% | Temp: %.2fC\n",
                  hr, spo2, temp);
    Serial.printf("Cluster: %d | Risk: %.2f | %s\n",
                  cluster, risk, severity(risk));
    Serial.println("--------------------");
  }
}