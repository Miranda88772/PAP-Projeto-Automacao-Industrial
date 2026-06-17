#include <WiFi.h>
#include <ThingerESP32.h>
#include <DHT.h>
#include <Wire.h>
#include <Adafruit_BMP3XX.h>
#include <U8g2lib.h>
#include <time.h>
#include <ESP32Servo.h>  // Usa esta library específica para ESP32 (instala via Library Manager)

#define USERNAME "Miranda88"
#define DEVICE_ID "miranda88-dht-01"
#define DEVICE_CREDENTIAL "mrnda-cred-3a9f6b2c"
#define WIFI_SSID "iPhone de Miranda"
#define WIFI_PASSWORD "Miranda88"
#define BUCKET_ID_DHT "dht_sensor_data"
#define BUCKET_ID_BMP "bmp_sensor_data"
#define VENT2_PIN 21
#define DHT_PIN 4
#define DHT_TYPE DHT11
// LEDs de estado
#define LED_VERDE 13
#define LED_AMARELO 12
#define LED_VERMELHO 14
// Buzzer
#define BUZZER_PIN 5
// OLED (software I2C)
#define OLED_SCL 32
#define OLED_SDA 33
// Controles rotativos + botões
#define BTN_CONFIRM 34
#define BTN_BACK 23
#define ROTARY_CLK 26
#define ROTARY_DT 27
// Porta automática + iluminação
#define TOUCH_PIN 2
#define PIR_PIN 35
#define SERVO_PIN 18
#define ILUM_PIN 15

ThingerESP32 thing(USERNAME, DEVICE_ID, DEVICE_CREDENTIAL);
DHT dht(DHT_PIN, DHT_TYPE);
Adafruit_BMP3XX bmp;
Servo portaServo;
U8G2_SH1106_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, OLED_SCL, OLED_SDA, U8X8_PIN_NONE);

// Variáveis sensores
#define NUM_SAMPLES 5
float dhtTempSamples[NUM_SAMPLES] = {0};
float dhtHumSamples[NUM_SAMPLES] = {0};
float bmpTempSamples[NUM_SAMPLES] = {0};
float bmpPressureSamples[NUM_SAMPLES] = {0};
int sampleIndex = 0;
unsigned long lastDHT = 0;
const unsigned long DHT_INTERVAL = 2000UL;
const unsigned long PRINT_INTERVAL = 5000UL;
bool vent1 = false;
bool bmp_available = false;
unsigned long lastBmpInitAttempt = 0;
const unsigned long BMP_INIT_RETRY_MS = 5000UL;

// Porta + Iluminação
bool portaAberta = false;
bool iluminacaoAcesa = false;
unsigned long ultimoToque = 0;
unsigned long portaAbertaTime = 0;
const unsigned long MAX_TEMPO_ABERTA = 10000UL;   // 10 segundos
bool movimentoDetectadoNaJanela = false;
unsigned long ultimoEstadoPIR = 0;                // Para detetar apenas transições LOW→HIGH

// Menu OLED
#define STATE_SPLASH 0
#define STATE_MENU 1
#define STATE_DHT 2
#define STATE_BMP 3
#define STATE_TEMPO 4
#define STATE_VENT 5
#define STATE_ILUM 6
int displayState = STATE_SPLASH;
int menuSelection = 0;
unsigned long lastInputTime = 0;
const unsigned long INPUT_DEBOUNCE = 100UL;
const unsigned long MENU_TIMEOUT = 40000UL;
int lastCLK = HIGH;
float dispTempDHT = 0, dispHumDHT = 0;
float dispTempBMP = NAN, dispPressBMP = NAN;

// ────────────────────────────────────────────────
// Funções Thinger
// ────────────────────────────────────────────────
void setupThinger() {
  thing["vent1"] << [](pson &in){
    if(in.is_empty()){
      in = vent1 ? 1 : 0;
    } else {
      bool state = (bool)in;
      applyFanHW(state, "thinger_io_individual");
    }
  };
  thing["vent1_state"] >> [](pson &out){ out = vent1 ? 1 : 0; };

  thing["dht"] >> [](pson &out){
    out["temperature"] = dispTempDHT;
    out["humidity"] = dispHumDHT;
  };
  thing["bmp388"] >> [](pson &out){
    out["bmp_temperature"] = isnan(dispTempBMP) ? -999 : dispTempBMP;
    out["pressure"] = isnan(dispPressBMP) ? -999 : dispPressBMP;
  };

  thing["Iluminacao_Ventoinha"] << [](pson &in){
    if(in.is_empty()){
      in = iluminacaoAcesa;
    } else {
      bool state = (bool)in;
      iluminacaoAcesa = state;
      digitalWrite(ILUM_PIN, state ? HIGH : LOW);
      applyFanHW(state, "thinger_io_conjunto");
      Serial.print("[THINGER] Luz + Vent -> ");
      Serial.println(state ? "LIGADAS" : "DESLIGADAS");
    }
  };

  thing["Iluminacao_state"] >> [](pson &out){ out = iluminacaoAcesa ? 1 : 0; };
}

// ────────────────────────────────────────────────
// Funções auxiliares
// ────────────────────────────────────────────────
void applyFanHW(bool ligar, const char* modo) {
  digitalWrite(VENT2_PIN, ligar ? HIGH : LOW);
  vent1 = ligar;
  pson p;
  p["modo"] = modo;
  p["estado"] = ligar ? 1 : 0;
  if (ligar) thing.call_endpoint("PAP_Aviso_ventoinha", p);
  else thing.call_endpoint("PAP_Aviso_ventoinha2", p);
  Serial.print("[VENT] Alterada para ");
  Serial.println(ligar ? "LIGADA" : "DESLIGADA");
}

void tryInitBMP() {
  if (bmp_available) return;
  Serial.println("[BMP] Tentando inicializar (0x76 ou 0x77)...");
  if (bmp.begin_I2C(0x76) || bmp.begin_I2C(0x77)) {
    bmp_available = true;
    bmp.setTemperatureOversampling(BMP3_OVERSAMPLING_8X);
    bmp.setPressureOversampling(BMP3_OVERSAMPLING_8X);
    bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_COEFF_3);
    bmp.setOutputDataRate(BMP3_ODR_50_HZ);
    Serial.println("[BMP] Inicializado com sucesso!");
  } else {
    Serial.println("[BMP] Falha na inicialização!");
  }
}

void printSensorData() {
  Serial.println("=======================================");
  Serial.print("DHT11 -> Temp: "); Serial.print(dispTempDHT,2); Serial.print(" °C | Hum: "); Serial.print(dispHumDHT,2); Serial.println("%");
  Serial.print("BMP388 -> Temp: "); Serial.print(isnan(dispTempBMP) ? "N/A" : String(dispTempBMP,2));
  Serial.print(" °C | Pressão: "); Serial.println(isnan(dispPressBMP) ? "N/A" : String(dispPressBMP/100.0,1) + " hPa");
  Serial.println("=======================================");
}

float calculateAverage(float samples[]) {
  float sum = 0;
  for (int i = 0; i < NUM_SAMPLES; i++) sum += samples[i];
  return sum / NUM_SAMPLES;
}

void updateLEDsAndBuzzer(float temp) {
  digitalWrite(LED_VERDE, LOW);
  digitalWrite(LED_AMARELO, LOW);
  digitalWrite(LED_VERMELHO, LOW);
  static bool alertaAtivo = false;
  if (temp < 20.0) {
    digitalWrite(LED_VERDE, HIGH);
    alertaAtivo = false;
  } else if (temp >= 20.0 && temp <= 29.9) {
    digitalWrite(LED_AMARELO, HIGH);
    alertaAtivo = false;
  } else if (temp >= 30.0) {
    digitalWrite(LED_VERMELHO, HIGH);
    if (!alertaAtivo) {
      for (int i = 0; i < 5; i++) {
        digitalWrite(BUZZER_PIN, HIGH); delay(200);
        digitalWrite(BUZZER_PIN, LOW); delay(200);
      }
      alertaAtivo = true;
    }
  }
}

// ────────────────────────────────────────────────
// OLED
// ────────────────────────────────────────────────
void updateOLED() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);
  time_t now_time = time(nullptr);
  struct tm *tm = localtime(&now_time);
  char dateBuf[20], timeBuf[10];
  sprintf(dateBuf, "%02d/%02d/%d", tm->tm_mday, tm->tm_mon + 1, tm->tm_year + 1900);
  sprintf(timeBuf, "%02d:%02d", tm->tm_hour, tm->tm_min);

  if (displayState == STATE_SPLASH) {
    u8g2.drawFrame(10, 5, 108, 50);
    u8g2.setFont(u8g2_font_ncenB14_tr);
    int x = (128 - u8g2.getStrWidth("AQUA")) / 2;
    u8g2.drawStr(x, 30, "AQUA");
    x = (128 - u8g2.getStrWidth("METEO")) / 2;
    u8g2.drawStr(x, 50, "METEO");
    u8g2.drawHLine(10, 52, 108);
    u8g2.setFont(u8g2_font_6x10_tr);
    x = (128 - u8g2.getStrWidth(dateBuf)) / 2;
    u8g2.drawStr(x, 15, dateBuf);
    x = (128 - u8g2.getStrWidth(timeBuf)) / 2;
    u8g2.drawStr(x, 62, timeBuf);
  } 
  else if (displayState == STATE_MENU) {
    u8g2.drawStr(40, 8, "MENU");
    u8g2.drawStr(85, 8, timeBuf);

    const char* itens[] = {"DHT11", "BMP388", "Tempo Atual", "Ventoinha", "Luz + Vent"};
    for (int i = 0; i < 5; i++) {
      if (i == menuSelection) u8g2.drawStr(8, 16 + i*10, ">");
      u8g2.drawStr(20, 16 + i*10, itens[i]);
    }
  } 
  else if (displayState == STATE_DHT) {
    u8g2.drawStr(35, 10, "DHT11");
    char buf[20];
    sprintf(buf, "Temp: %.1f C", dispTempDHT);
    u8g2.drawStr(15, 30, buf);
    sprintf(buf, "Hum: %.1f %%", dispHumDHT);
    u8g2.drawStr(15, 45, buf);
    u8g2.drawStr(30, 60, "BACK voltar");
  } 
  else if (displayState == STATE_BMP) {
    u8g2.drawStr(30, 10, "BMP388");
    char buf[25];
    if (isnan(dispTempBMP)) u8g2.drawStr(10, 28, "Temp: N/A");
    else {
      sprintf(buf, "Temp: %.1f C", dispTempBMP);
      u8g2.drawStr(10, 28, buf);
    }
    if (isnan(dispPressBMP)) u8g2.drawStr(10, 45, "Pres: N/A");
    else {
      sprintf(buf, "Pres: %.0f hPa", dispPressBMP / 100.0);
      u8g2.drawStr(10, 45, buf);
    }
    u8g2.drawStr(30, 60, "BACK voltar");
  } 
  else if (displayState == STATE_TEMPO) {
    u8g2.drawStr(15, 10, "Tempo Atual");
    char buf[30];
    sprintf(buf, "Temp: %.1f C", dispTempDHT);
    u8g2.drawStr(5, 25, buf);
    sprintf(buf, "Hum: %.1f %%", dispHumDHT);
    u8g2.drawStr(5, 37, buf);
    if (isnan(dispPressBMP)) u8g2.drawStr(5, 49, "Pres: N/A");
    else {
      sprintf(buf, "Pres: %.0f hPa", dispPressBMP / 100.0);
      u8g2.drawStr(5, 49, buf);
    }
    u8g2.drawStr(30, 60, "BACK voltar");
  }
  else if (displayState == STATE_VENT) {
    u8g2.drawStr(20, 15, "Ventoinha");
    u8g2.setFont(u8g2_font_ncenB10_tr);
    u8g2.drawStr(30, 40, vent1 ? "LIGADA" : "DESLIGADA");
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(5, 60, "CONFIRM toggle BACK");
  } 
  else if (displayState == STATE_ILUM) {
    u8g2.drawStr(30, 15, "Luz + Vent");
    u8g2.setFont(u8g2_font_ncenB10_tr);
    u8g2.drawStr(30, 40, iluminacaoAcesa ? "LIGADAS" : "DESLIGADAS");
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(5, 60, "CONFIRM toggle BACK");
  }
  u8g2.sendBuffer();
}

// ────────────────────────────────────────────────
// Tratamento de botões + rotary
// ────────────────────────────────────────────────
void handleInputs() {
  unsigned long now = millis();
  if (now - lastInputTime < INPUT_DEBOUNCE) return;
  bool inputDetected = false;

  int currentCLK = digitalRead(ROTARY_CLK);
  if (currentCLK != lastCLK && currentCLK == LOW) {
    if (digitalRead(ROTARY_DT) != currentCLK) 
      menuSelection = (menuSelection + 1) % 5;
    else 
      menuSelection = (menuSelection - 1 + 5) % 5;
    inputDetected = true;
    if (displayState == STATE_MENU) updateOLED();
  }
  lastCLK = currentCLK;

  if (digitalRead(BTN_CONFIRM) == LOW) {
    inputDetected = true;
    if (displayState == STATE_SPLASH) displayState = STATE_MENU;
    else if (displayState == STATE_MENU) {
      if (menuSelection == 0) displayState = STATE_DHT;
      else if (menuSelection == 1) displayState = STATE_BMP;
      else if (menuSelection == 2) displayState = STATE_TEMPO;
      else if (menuSelection == 3) displayState = STATE_VENT;
      else if (menuSelection == 4) displayState = STATE_ILUM;
    } 
    else if (displayState == STATE_VENT) {
      applyFanHW(!vent1, "manual_oled");
    } 
    else if (displayState == STATE_ILUM) {
      iluminacaoAcesa = !iluminacaoAcesa;
      digitalWrite(ILUM_PIN, iluminacaoAcesa ? HIGH : LOW);
      applyFanHW(iluminacaoAcesa, "manual_oled_conjunto");
      Serial.print("[OLED] Luz + Vent ");
      Serial.println(iluminacaoAcesa ? "LIGADAS" : "DESLIGADAS");
    }
    updateOLED();
    delay(200);
  }

  if (digitalRead(BTN_BACK) == LOW) {
    inputDetected = true;
    if (displayState >= STATE_DHT && displayState <= STATE_ILUM) {
      displayState = STATE_MENU;
      menuSelection = 0;
      updateOLED();
    } else if (displayState == STATE_MENU) {
      displayState = STATE_SPLASH;
      updateOLED();
    }
    delay(200);
  }

  if (inputDetected) lastInputTime = now;
}

// ────────────────────────────────────────────────
// Verifica se é horário noturno
// ────────────────────────────────────────────────
bool ehNoite() {
  time_t now_time = time(nullptr);
  struct tm *tm = localtime(&now_time);
  int hora = tm->tm_hour;
  return (hora >= 20 || hora < 9);
}

// ────────────────────────────────────────────────
// SETUP
// ────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("Estação Híbrida Pequena Fechada - Iniciada!");

  pinMode(VENT2_PIN, OUTPUT); digitalWrite(VENT2_PIN, LOW);
  pinMode(LED_VERDE, OUTPUT); digitalWrite(LED_VERDE, LOW);
  pinMode(LED_AMARELO, OUTPUT); digitalWrite(LED_AMARELO, LOW);
  pinMode(LED_VERMELHO, OUTPUT); digitalWrite(LED_VERMELHO, LOW);
  pinMode(BUZZER_PIN, OUTPUT); digitalWrite(BUZZER_PIN, LOW);
  pinMode(BTN_CONFIRM, INPUT_PULLUP);
  pinMode(BTN_BACK, INPUT_PULLUP);
  pinMode(ROTARY_CLK, INPUT_PULLUP);
  pinMode(ROTARY_DT, INPUT_PULLUP);
  pinMode(TOUCH_PIN, INPUT);
  pinMode(PIR_PIN, INPUT);
  pinMode(ILUM_PIN, OUTPUT); digitalWrite(ILUM_PIN, LOW);

  portaServo.attach(SERVO_PIN);
  portaServo.write(0);
  portaAberta = false;

  dht.begin();
  Wire.begin(19, 22);
  tryInitBMP();
  lastBmpInitAttempt = millis();

  u8g2.begin();
  updateOLED();

  vent1 = digitalRead(VENT2_PIN) == HIGH;

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED && millis() < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? "\nWiFi OK" : "\nWiFi Falhou");

  if (WiFi.status() == WL_CONNECTED) {
    configTime(0, 0, "pool.ntp.org");
    setenv("TZ", "WET0WEST,M3.5.0/1,M10.5.0", 1);
    tzset();
    Serial.println("Hora NTP sincronizada!");
  }

  thing.add_wifi(WIFI_SSID, WIFI_PASSWORD);
  setupThinger();

  lastInputTime = millis();
}

// ────────────────────────────────────────────────
// LOOP PRINCIPAL
// ────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();
  thing.handle();

  if (!bmp_available && now - lastBmpInitAttempt >= BMP_INIT_RETRY_MS) {
    lastBmpInitAttempt = now;
    tryInitBMP();
  }

  if (displayState != STATE_SPLASH && now - lastInputTime > MENU_TIMEOUT) {
    displayState = STATE_SPLASH;
    menuSelection = 0;
    updateOLED();
  }

  if (now - lastDHT >= DHT_INTERVAL) {
    lastDHT = now;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    float bt = NAN, bp = NAN;
    if (bmp_available && bmp.performReading()) {
      bt = bmp.temperature;
      bp = bmp.pressure;
    }
    if (!isnan(t) && !isnan(h)) {
      dhtTempSamples[sampleIndex] = t;
      dhtHumSamples[sampleIndex] = h;
    }
    if (!isnan(bt) && !isnan(bp)) {
      bmpTempSamples[sampleIndex] = bt;
      bmpPressureSamples[sampleIndex] = bp;
    }
    sampleIndex = (sampleIndex + 1) % NUM_SAMPLES;
    if (sampleIndex == 0) {
      dispTempDHT = calculateAverage(dhtTempSamples);
      dispHumDHT = calculateAverage(dhtHumSamples);
      dispTempBMP = calculateAverage(bmpTempSamples);
      dispPressBMP = calculateAverage(bmpPressureSamples);
      thing.write_bucket(BUCKET_ID_DHT, "dht");
      thing.write_bucket(BUCKET_ID_BMP, "bmp388");
      updateLEDsAndBuzzer(dispTempDHT);
    }
  }

  static unsigned long lastPrint = 0;
  if (now - lastPrint >= PRINT_INTERVAL) {
    printSensorData();
    lastPrint = now;
  }

  handleInputs();

  static unsigned long lastRefresh = 0;
  if (now - lastRefresh >= 1000) {
    updateOLED();
    lastRefresh = now;
  }

  // ─── LÓGICA DA PORTA (corrigida e otimizada) ────────────────────────────────
  static unsigned long ultimoEstadoPIR = 0;

  // 1. Abrir com toque (só se estiver fechada)
  if (!portaAberta && digitalRead(TOUCH_PIN) == HIGH && now - ultimoToque > 600) {
    ultimoToque = now;
    Serial.println("[PORTA] Toque detetado → ABRINDO");
    portaServo.write(180);
    portaAberta = true;
    portaAbertaTime = now;
    movimentoDetectadoNaJanela = false;
    ultimoEstadoPIR = 0;
    delay(300);
  }

  // 2. Enquanto a porta está aberta
  if (portaAberta) {
    bool pirAtual = digitalRead(PIR_PIN);

    // Deteta apenas **nova** transição LOW → HIGH (novo movimento)
    if (pirAtual == HIGH && ultimoEstadoPIR == LOW) {
      Serial.println("[PIR] Novo movimento detetado → reiniciando 10s");
      movimentoDetectadoNaJanela = true;
      portaAbertaTime = now;  // REINICIA o temporizador
    }

    ultimoEstadoPIR = pirAtual;

    // Se passaram 10 segundos **sem qualquer novo movimento**
    if (now - portaAbertaTime >= MAX_TEMPO_ABERTA) {
      Serial.println("[PORTA] 10 segundos sem movimento novo → FECHANDO");
      portaServo.write(0);
      portaAberta = false;
      delay(300);
      // NÃO altera iluminação nem ventoinha!
    }
  }
}
