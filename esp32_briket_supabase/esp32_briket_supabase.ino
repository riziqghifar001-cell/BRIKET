#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>

// =========================================================
// 1. KONFIGURASI WIFI & SUPABASE (SESUAIKAN DENGAN PUNYAMU)
// =========================================================
const char* WIFI_SSID         = "NAMA_WIFI_KAMU";
const char* WIFI_PASS         = "PASSWORD_WIFI_KAMU";
const char* SUPABASE_URL      = "https://ejfftyrdmcrdkqvxxhxy.supabase.co";
const char* SUPABASE_ANON_KEY = "sb_publishable_Zf7P8fH6ULcn2ll90UVMBA_IG7umqkn";

// =========================================================
// 2. KONFIGURASI PIN HARDWARE ESP32 (SESUAIKAN WIRING PCB)
// =========================================================
#define PIN_RELAY_WIPER   26   // Relay CH-1: Motor Pemotong Wiper
#define PIN_RELAY_AC      27   // Relay CH-2: Kontaktor / Motor AC Ekstruder
#define PIN_SENSOR_PROX   34   // Sensor Proximity (INPUT_PULLUP jika NPN, atau eksternal pull-up)
#define PIN_EMERGENCY     35   // Saklar Push-Lock Darurat
#define PIN_BUZZER        25   // Buzzer Alarm Darurat
#define PIN_BTN_START     32   // Tombol Fisik START (opsional, aktif LOW ke GND)
#define PIN_BTN_STOP      33   // Tombol Fisik STOP  (opsional, aktif LOW ke GND)

// Ubah ke false jika modul relay aktif HIGH
#define RELAY_ACTIVE_LOW  true

// Logika input sensor & tombol darurat:
// true  = aktif saat sinyal LOW (misal sensor NPN / saklar ke GND)
// false = aktif saat sinyal HIGH
#define PROX_ACTIVE_LOW   true
#define EMERG_ACTIVE_LOW  true

// =========================================================
// 3. PARAMETER TIMING PROSES BRIKET
// =========================================================
const unsigned long CUT_PAUSE_MS      = 3000;  // Motor AC jeda 3 detik saat potong
const unsigned long WIPER_RUN_MS      = 1500;  // Durasi relay wiper aktif (1x putaran pisau)
const unsigned long PROX_DEBOUNCE_MS  = 50;    // Filter noise induktif motor AC pada sensor
const unsigned long SYNC_INTERVAL_MS  = 1500;  // Interval sinkronisasi rutin ke Supabase

// =========================================================
// VARIABEL GLOBAL & SINKRONISASI ANTAR-CORE
// =========================================================
enum MachineState {
  STATE_STOPPED,
  STATE_RUNNING,
  STATE_CUTTING,
  STATE_EMERGENCY
};

Preferences prefs;
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;

volatile MachineState currentState = STATE_STOPPED;
volatile uint32_t briketCount      = 0;
volatile bool ch1WiperOn           = false;
volatile bool ch2AcOn              = false;
volatile bool emergencyActive      = false;
volatile bool syncImmediate        = true;

unsigned long cutStartMs           = 0;
bool proxLatched                   = false;
unsigned long lastProxChangeMs     = 0;
bool lastRawProx                   = false;

// =========================================================
// HELPER AKTUATOR & NVS
// =========================================================
void setRelay(uint8_t pin, bool on) {
  if (RELAY_ACTIVE_LOW) {
    digitalWrite(pin, on ? LOW : HIGH);
  } else {
    digitalWrite(pin, on ? HIGH : LOW);
  }
}

void saveCountToNVS(uint32_t val) {
  prefs.putUInt("count", val);
}

const char* stateToString(MachineState st) {
  switch (st) {
    case STATE_RUNNING:   return "RUNNING";
    case STATE_CUTTING:   return "CUTTING";
    case STATE_EMERGENCY: return "EMERGENCY";
    default:              return "STOPPED";
  }
}

void applyOutputs(MachineState st, unsigned long now) {
  if (st == STATE_EMERGENCY) {
    ch1WiperOn = false;
    ch2AcOn    = false;
    setRelay(PIN_RELAY_WIPER, false);
    setRelay(PIN_RELAY_AC, false);
    // Buzzer cepat (150ms ON / 150ms OFF)
    digitalWrite(PIN_BUZZER, ((now / 150) % 2 == 0) ? HIGH : LOW);
  } else if (st == STATE_CUTTING) {
    ch2AcOn = false;
    setRelay(PIN_RELAY_AC, false);

    bool wiperShouldRun = (now - cutStartMs) < WIPER_RUN_MS;
    ch1WiperOn = wiperShouldRun;
    setRelay(PIN_RELAY_WIPER, wiperShouldRun);
    digitalWrite(PIN_BUZZER, LOW);
  } else if (st == STATE_RUNNING) {
    ch1WiperOn = false;
    ch2AcOn    = true;
    setRelay(PIN_RELAY_WIPER, false);
    setRelay(PIN_RELAY_AC, true);
    digitalWrite(PIN_BUZZER, LOW);
  } else {
    ch1WiperOn = false;
    ch2AcOn    = false;
    setRelay(PIN_RELAY_WIPER, false);
    setRelay(PIN_RELAY_AC, false);
    digitalWrite(PIN_BUZZER, LOW);
  }
}

void commandStart() {
  portENTER_CRITICAL(&stateMux);
  if (!emergencyActive && currentState == STATE_STOPPED) {
    currentState = STATE_RUNNING;
    proxLatched = false;
    syncImmediate = true;
  }
  portEXIT_CRITICAL(&stateMux);
}

void commandStop() {
  portENTER_CRITICAL(&stateMux);
  if (!emergencyActive && currentState != STATE_STOPPED) {
    currentState = STATE_STOPPED;
    syncImmediate = true;
  }
  portEXIT_CRITICAL(&stateMux);
}

void commandResetCount() {
  portENTER_CRITICAL(&stateMux);
  briketCount = 0;
  syncImmediate = true;
  portEXIT_CRITICAL(&stateMux);
  saveCountToNVS(0);
}

// =========================================================
// PARSER JSON RINGAN (TANPA DEPENDENSI EKSTERNAL)
// =========================================================
String extractJsonField(const String& json, const String& key) {
  String pattern = "\"" + key + "\":";
  int idx = json.indexOf(pattern);
  if (idx < 0) return "";
  idx += pattern.length();
  while (idx < (int)json.length() && (json[idx] == ' ' || json[idx] == '\"')) {
    idx++;
  }
  int endIdx = idx;
  while (endIdx < (int)json.length() &&
         json[endIdx] != '\"' &&
         json[endIdx] != ',' &&
         json[endIdx] != '}' &&
         json[endIdx] != ']') {
    endIdx++;
  }
  return json.substring(idx, endIdx);
}

// =========================================================
// TASK CORE 0: SINKRONISASI SUPABASE (NON-BLOCKING KE MESIN)
// =========================================================
void supabaseSyncTask(void* pvParameters) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long lastSyncMs = 0;
  bool initialCloudPulled = false;

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    unsigned long now = millis();
    bool force = false;

    portENTER_CRITICAL(&stateMux);
    force = syncImmediate;
    portEXIT_CRITICAL(&stateMux);

    if (!force && (now - lastSyncMs < SYNC_INTERVAL_MS)) {
      vTaskDelay(pdMS_TO_TICKS(80));
      continue;
    }

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    String endpoint = String(SUPABASE_URL) + "/rest/v1/mesin_briket?id=eq.1";

    // 1. Ambil perintah terbaru dari Web HMI (GET)
    if (http.begin(client, endpoint + "&select=count,cmd")) {
      http.addHeader("apikey", SUPABASE_ANON_KEY);

      int code = http.GET();
      if (code == 200) {
        String body = http.getString();
        String cmdVal = extractJsonField(body, "cmd");
        String cntVal = extractJsonField(body, "count");

        // Sinkronisasi counter awal dari cloud jika NVS lokal masih 0
        if (!initialCloudPulled && cntVal.length() > 0) {
          uint32_t cloudCount = (uint32_t)cntVal.toInt();
          portENTER_CRITICAL(&stateMux);
          if (briketCount == 0 && cloudCount > 0) {
            briketCount = cloudCount;
            saveCountToNVS(briketCount);
          }
          portEXIT_CRITICAL(&stateMux);
          initialCloudPulled = true;
        }

        if (cmdVal == "START") {
          commandStart();
        } else if (cmdVal == "STOP") {
          commandStop();
        } else if (cmdVal == "RESET_CNT") {
          commandResetCount();
        }
      }
      http.end();
    }

    // 2. Kirim status aktual mesin & bersihkan kolom cmd -> NONE (PATCH)
    MachineState snapState;
    uint32_t snapCount;
    bool snapCh1, snapCh2, snapEmerg;

    portENTER_CRITICAL(&stateMux);
    snapState     = currentState;
    snapCount     = briketCount;
    snapCh1       = ch1WiperOn;
    snapCh2       = ch2AcOn;
    snapEmerg     = emergencyActive;
    syncImmediate = false;
    portEXIT_CRITICAL(&stateMux);

    String payload = "{";
    payload += "\"state\":\"" + String(stateToString(snapState)) + "\",";
    payload += "\"count\":" + String(snapCount) + ",";
    payload += "\"ch1_wiper\":" + String(snapCh1 ? "true" : "false") + ",";
    payload += "\"ch2_ac\":" + String(snapCh2 ? "true" : "false") + ",";
    payload += "\"emergency\":" + String(snapEmerg ? "true" : "false") + ",";
    payload += "\"cmd\":\"NONE\",";
    payload += "\"updated_at\":\"now()\"";
    payload += "}";

    if (http.begin(client, endpoint)) {
      http.addHeader("apikey", SUPABASE_ANON_KEY);
      http.addHeader("Content-Type", "application/json");
      http.addHeader("Prefer", "return=minimal");

      http.PATCH(payload);
      http.end();
    }

    lastSyncMs = millis();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// =========================================================
// SETUP & CORE 1 REAL-TIME CONTROL LOOP
// =========================================================
void setup() {
  Serial.begin(115200);

  pinMode(PIN_RELAY_WIPER, OUTPUT);
  pinMode(PIN_RELAY_AC, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);

  setRelay(PIN_RELAY_WIPER, false);
  setRelay(PIN_RELAY_AC, false);
  digitalWrite(PIN_BUZZER, LOW);

  pinMode(PIN_SENSOR_PROX, PROX_ACTIVE_LOW ? INPUT_PULLUP : INPUT);
  pinMode(PIN_EMERGENCY,   EMERG_ACTIVE_LOW ? INPUT_PULLUP : INPUT);
  pinMode(PIN_BTN_START,   INPUT_PULLUP);
  pinMode(PIN_BTN_STOP,    INPUT_PULLUP);

  prefs.begin("briket_nvs", false);
  briketCount = prefs.getUInt("count", 0);

  // Jalankan task jaringan Supabase di Core 0
  xTaskCreatePinnedToCore(
    supabaseSyncTask,
    "SupabaseSync",
    8192,
    NULL,
    1,
    NULL,
    0
  );
}

void loop() {
  unsigned long now = millis();

  // 1. Cek Saklar Darurat Push-Lock (Prioritas Tertinggi)
  bool rawEmerg = (digitalRead(PIN_EMERGENCY) == (EMERG_ACTIVE_LOW ? LOW : HIGH));

  portENTER_CRITICAL(&stateMux);
  if (rawEmerg && !emergencyActive) {
    emergencyActive = true;
    currentState = STATE_EMERGENCY;
    syncImmediate = true;
  } else if (!rawEmerg && emergencyActive) {
    emergencyActive = false;
    currentState = STATE_STOPPED;
    syncImmediate = true;
  }
  portEXIT_CRITICAL(&stateMux);

  // 2. Cek Tombol Fisik START / STOP (Jika terpasang pada panel boks)
  if (!emergencyActive) {
    if (digitalRead(PIN_BTN_START) == LOW) {
      commandStart();
    } else if (digitalRead(PIN_BTN_STOP) == LOW) {
      commandStop();
    }
  }

  // 3. Debounce Sensor Proximity
  bool rawProx = (digitalRead(PIN_SENSOR_PROX) == (PROX_ACTIVE_LOW ? LOW : HIGH));
  if (rawProx != lastRawProx) {
    lastProxChangeMs = now;
    lastRawProx = rawProx;
  }
  bool stableProx = ((now - lastProxChangeMs) >= PROX_DEBOUNCE_MS) ? rawProx : proxLatched;
  if (!stableProx) {
    proxLatched = false;
  }

  // 4. State Machine Utama Mesin Briket
  MachineState st;
  portENTER_CRITICAL(&stateMux);
  st = currentState;
  portEXIT_CRITICAL(&stateMux);

  if (st == STATE_RUNNING) {
    if (stableProx && !proxLatched) {
      proxLatched = true;
      cutStartMs = now;

      uint32_t updatedCount = 0;
      portENTER_CRITICAL(&stateMux);
      briketCount++;
      updatedCount = briketCount;
      currentState = STATE_CUTTING;
      syncImmediate = true;
      portEXIT_CRITICAL(&stateMux);

      saveCountToNVS(updatedCount);
    }
  } else if (st == STATE_CUTTING) {
    if (now - cutStartMs >= CUT_PAUSE_MS) {
      portENTER_CRITICAL(&stateMux);
      if (currentState == STATE_CUTTING) {
        currentState = STATE_RUNNING;
        syncImmediate = true;
      }
      portEXIT_CRITICAL(&stateMux);
    }
  }

  // 5. Terapkan Output Relay & Buzzer
  portENTER_CRITICAL(&stateMux);
  st = currentState;
  applyOutputs(st, now);
  portEXIT_CRITICAL(&stateMux);

  delay(2);
}

