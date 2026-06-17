 #include <SPI.h>
#include <MFRC522.h>
#include <Keypad.h>
#include <Wire.h>
#include <U8g2lib.h>

// ================= DISPLAY =================
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// ================= BOTÕES E ENCODER =================
#define BTN_OK    34
#define BTN_BACK  33
#define ENC_CLK   32
#define ENC_DT    31          // Pino corrigido (não conflita com botão)

int ultimoCLK;
int menuIndex = 0;
bool emMenu   = false;
bool emTopico = false;
bool splash   = true;

// ================= RELÓGIO (baseado na data de compilação) =================
unsigned long ultimoTempo = 0;
int dia, mes, ano, hora, minuto, segundo;

void inicializarRelogio() {
  const char* data = __DATE__;
  const char* tempo = __TIME__;
  char mesStr[4];
  sscanf(data, "%s %d %d", mesStr, &dia, &ano);

  if      (strcmp(mesStr, "Jan")==0) mes=1;
  else if (strcmp(mesStr, "Feb")==0) mes=2;
  else if (strcmp(mesStr, "Mar")==0) mes=3;
  else if (strcmp(mesStr, "Apr")==0) mes=4;
  else if (strcmp(mesStr, "May")==0) mes=5;
  else if (strcmp(mesStr, "Jun")==0) mes=6;
  else if (strcmp(mesStr, "Jul")==0) mes=7;
  else if (strcmp(mesStr, "Aug")==0) mes=8;
  else if (strcmp(mesStr, "Sep")==0) mes=9;
  else if (strcmp(mesStr, "Oct")==0) mes=10;
  else if (strcmp(mesStr, "Nov")==0) mes=11;
  else if (strcmp(mesStr, "Dec")==0) mes=12;

  sscanf(tempo, "%d:%d:%d", &hora, &minuto, &segundo);
}

void atualizarRelogio() {
  if (millis() - ultimoTempo >= 1000) {
    ultimoTempo = millis();
    segundo++;
    if (segundo >= 60) { segundo = 0; minuto++; }
    if (minuto  >= 60) { minuto  = 0; hora++;   }
    if (hora    >= 24) { hora    = 0;           }
  }
}

// ================= MENU PRINCIPAL =================
// "Inserir PIN" é agora o PRIMEIRO item (índice 0)
const char* menuItems[] = {
  "Inserir PIN",          // 0 - prioridade máxima
  "Estado do Portao",
  "Sistema RFID",
  "Sensor IR",
  "Informacoes",
  "LOGS",
  "Redefinir PIN"
};
const int totalMenu = 7;   // aumentou 1 item

// ================= LOGS =================
String logs[5];
int logIndex = 0;

void adicionarLog(bool abriu) {
  char buf[24];
  sprintf(buf, "%s as %02d:%02d", abriu ? "Aberto" : "Fechado", hora, minuto);
  logs[logIndex] = buf;
  logIndex = (logIndex + 1) % 5;
}

// ================= RFID =================
#define SS_PIN 53
#define RST_PIN A5
MFRC522 rfid(SS_PIN, RST_PIN);
byte authorizedUID[] = {0xE7, 0x90, 0x31, 0x07};

// ================= KEYPAD =================
const byte ROWS = 4, COLS = 4;
char keys[ROWS][COLS] = {{'1','2','3','A'}, {'4','5','6','B'}, {'7','8','9','C'}, {'*','0','#','D'}};
byte rowPins[ROWS] = {22,23,24,25};
byte colPins[COLS] = {26,27,28,29};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

String codigoDigitado = "";           // código que o utilizador está a digitar agora
String codigoCorreto  = "1234";       // PIN padrão (pode ser alterado no menu Redefinir)

String adminPass = "8888";
String inputAdmin = "";
String inputNovoPIN = "";
bool aguardandoAdmin = false;
bool aguardandoNovoPIN = false;

// ================= MOTOR / SENSORES / LEDs / BUZZER =================
#define ENA 6
#define IN1 7
#define IN2 8
#define FIM_FRENTE 9
#define FIM_TRAS 10
#define SENSOR_IR 11
#define LED_VERMELHO 2
#define LED_AMARELO 3
#define LED_VERDE 4
#define BUZZER 44

unsigned long tempoSom = 0;
bool estadoSom = false;
const unsigned long intervaloSom = 300;

unsigned long tempoInicio = 0;
const unsigned long tempoEspera = 10000;
const unsigned long tempoPreAviso = 3000;

enum EstadoPortao { PARADO, ABRINDO, ABERTO, FECHANDO };
EstadoPortao estado = PARADO;
bool motorRodando = false;

// ================= FUNÇÕES DE CONTROLO DO MOTOR E LEDs =================
void stopMotor() {
  motorRodando = false;
  analogWrite(ENA, 0);
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  Serial.println("Motor parado.");
}

void startMotor(bool frente) {
  motorRodando = true;
  digitalWrite(IN1, frente);
  digitalWrite(IN2, !frente);
  analogWrite(ENA, 200);
  estado = frente ? ABRINDO : FECHANDO;
  atualizarLEDs();
  Serial.println(frente ? "Abrindo portão..." : "Fechando portão...");
}

void atualizarLEDs() {
  digitalWrite(LED_VERMELHO, LOW);
  digitalWrite(LED_AMARELO,  LOW);
  digitalWrite(LED_VERDE,    LOW);

  if (motorRodando) {
    digitalWrite(LED_AMARELO, HIGH);
  } else {
    if (estado == PARADO)  digitalWrite(LED_VERMELHO, HIGH);
    else if (estado == ABERTO) digitalWrite(LED_VERDE, HIGH);
  }
}

bool checkUID(byte *uid) {
  for (byte i = 0; i < 4; i++) if (uid[i] != authorizedUID[i]) return false;
  return true;
}

// ================= SONS =================
void somAbrindo() {
  if (millis() - tempoSom >= intervaloSom) {
    tempoSom = millis();
    estadoSom = !estadoSom;
    if (estadoSom) tone(BUZZER, 1800); else noTone(BUZZER);
  }
}

void somFechando() {
  if (millis() - tempoSom >= intervaloSom) {
    tempoSom = millis();
    estadoSom = !estadoSom;
    if (estadoSom) tone(BUZZER, 1000); else noTone(BUZZER);
  }
}

void somPreAviso() { tone(BUZZER, 1500, 800); }

void sireneIR() {
  for (int i = 0; i < 3; i++) {
    tone(BUZZER, 2800, 120); delay(150);
    tone(BUZZER, 2200, 120); delay(150);
  }
  noTone(BUZZER);
}

// ================= TELAS DO DISPLAY =================
static int barraProgresso = 0;

void telaSplash() {
  if (barraProgresso < 100) {
    barraProgresso += 2;
    if (barraProgresso > 100) barraProgresso = 100;
  }
  u8g2.clearBuffer();
  u8g2.drawRFrame(4, 4, 120, 56, 5);
  u8g2.setFont(u8g2_font_7x14B_tr);
  u8g2.drawStr(14, 22, "ETAR GATE PRO");
  u8g2.drawHLine(14, 26, 100);
  u8g2.setFont(u8g2_font_6x10_tf);
  char buf[24];
  sprintf(buf, "%02d/%02d/%d %02d:%02d:%02d", dia, mes, ano % 100, hora, minuto, segundo);
  u8g2.drawStr(14, 40, buf);
  u8g2.drawFrame(24, 50, 80, 6);
  u8g2.drawBox(24, 50, (barraProgresso * 80) / 100, 6);
  u8g2.sendBuffer();
}

void desenharMenu() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  char buf[10];
  sprintf(buf, "%02d:%02d", hora, minuto);
  u8g2.drawStr(88, 9, buf);
  u8g2.drawStr(4, 9, "MENU");
  u8g2.drawHLine(0, 12, 128);

  int start = menuIndex - 1;
  if (start < 0) start = 0;
  if (start > totalMenu - 3) start = totalMenu - 3;

  for (int i = 0; i < 3; i++) {
    int idx = start + i;
    if (idx >= totalMenu) break;
    int y = 26 + i * 13;
    if (idx == menuIndex) {
      u8g2.drawBox(0, y-10, 128, 12);
      u8g2.setDrawColor(0);
    }
    u8g2.drawStr(8, y, menuItems[idx]);
    if (idx == menuIndex) u8g2.setDrawColor(1);
  }
  u8g2.sendBuffer();
}

// Tela especial para inserir PIN (muito limpa e direta)
void desenharTelaInserirPIN() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_7x14B_tr);
  u8g2.drawStr(4, 18, "INSERIR PIN");
  u8g2.drawHLine(0, 22, 128);

  u8g2.setFont(u8g2_font_10x20_tf);   // fonte maior para os dígitos
  u8g2.drawStr(20, 45, codigoDigitado.c_str());

  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(4, 60, "Confirme com #   * apaga");
  u8g2.sendBuffer();
}

void desenharTopico() {
  // Se for o novo item "Inserir PIN" (menuIndex == 0)
  if (menuIndex == 0) {
    desenharTelaInserirPIN();
    return;
  }

  // Restantes tópicos normais
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tr);

  switch (menuIndex) {
    case 1: // Estado do Portao (agora índice 1)
      u8g2.drawStr(4, 14, "ESTADO DO PORTAO");
      u8g2.drawHLine(0, 17, 128);
      switch (estado) {
        case PARADO:   u8g2.drawStr(10, 32, "Fechado");     break;
        case ABRINDO:  u8g2.drawStr(10, 32, "Abrindo...");  break;
        case ABERTO:   u8g2.drawStr(10, 32, "Aberto");      break;
        case FECHANDO: u8g2.drawStr(10, 32, "Fechando..."); break;
      }
      break;

    case 2: u8g2.drawStr(4, 14, "SISTEMA RFID"); u8g2.drawStr(10, 32, "Ativo - 1 cartao"); break;
    case 3: u8g2.drawStr(4, 14, "SENSOR IR"); u8g2.drawStr(10, 32, digitalRead(SENSOR_IR)==LOW ? "Pessoa detectada" : "Livre"); break;

    case 4:
      u8g2.drawStr(4, 14, "ETAR GATE PRO");
      u8g2.drawStr(10, 28, "Versao 2.1");
      u8g2.drawStr(10, 42, "Joao & Rodrigo");
      break;

    case 5: // LOGS
      u8g2.drawStr(4, 14, "HISTORICO PORTAO");
      u8g2.drawHLine(0, 17, 128);
      u8g2.setFont(u8g2_font_6x10_tf);
      for (int i = 0; i < 5; i++) {
        int idx = (logIndex - 1 - i + 5) % 5;
        if (logs[idx].length()) u8g2.drawStr(6, 28 + i*10, logs[idx].c_str());
      }
      break;

    case 6: // Redefinir PIN (agora último)
      if (aguardandoAdmin) {
        u8g2.drawStr(4, 14, "Senha Admin:");
        u8g2.drawStr(10, 32, inputAdmin.c_str());
        u8g2.drawStr(10, 46, "Confirme com #");
      } else if (aguardandoNovoPIN) {
        u8g2.drawStr(4, 14, "Novo PIN:");
        u8g2.drawStr(10, 32, inputNovoPIN.c_str());
        u8g2.drawStr(10, 46, "Confirme com #");
      } else {
        u8g2.drawStr(4, 14, "REDEFINIR PIN");
        u8g2.drawStr(10, 32, "Pressione OK");
      }
      break;
  }

  if (menuIndex != 6 || (!aguardandoAdmin && !aguardandoNovoPIN))
    u8g2.drawStr(0, 63, "< BACK");

  u8g2.sendBuffer();
}

// ================= LEITURA DE BOTÕES =================
void lerBotoesDisplay() {
  static unsigned long dbOK = 0, dbBACK = 0;

  if (digitalRead(BTN_OK) == LOW && millis() - dbOK > 200) {
    dbOK = millis();
    if (splash) { splash = false; emMenu = true; }
    else if (emMenu && !emTopico) {
      emTopico = true;
      // Se entrarmos no "Inserir PIN" não precisamos de inicializar nada extra
      if (menuIndex == 6) {   // Redefinir PIN
        aguardandoAdmin = true;
        inputAdmin = "";
        inputNovoPIN = "";
      }
    }
  }

  if (digitalRead(BTN_BACK) == LOW && millis() - dbBACK > 200) {
    dbBACK = millis();
    if (emTopico) {
      emTopico = false;
      aguardandoAdmin = aguardandoNovoPIN = false;
      inputAdmin = inputNovoPIN = "";
      codigoDigitado = "";   // limpa também o PIN digitado
    } else if (emMenu && !emTopico) {
      emMenu = false;
      splash = true;
    }
  }
}

// ================= ENCODER =================
void lerEncoder() {
  int clk = digitalRead(ENC_CLK);
  if (clk != ultimoCLK && clk == LOW) {
    if (digitalRead(ENC_DT) != clk) menuIndex++;
    else menuIndex--;
    menuIndex = (menuIndex + totalMenu) % totalMenu;
  }
  ultimoCLK = clk;
}

// ================= SETUP =================
void setup() {
  Serial.begin(9600);
  SPI.begin();
  rfid.PCD_Init();
  u8g2.begin();

  pinMode(BTN_OK, INPUT_PULLUP);
  pinMode(BTN_BACK, INPUT_PULLUP);
  pinMode(ENC_CLK, INPUT);
  pinMode(ENC_DT, INPUT);
  ultimoCLK = digitalRead(ENC_CLK);

  pinMode(ENA, OUTPUT);
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(FIM_FRENTE, INPUT_PULLUP);
  pinMode(FIM_TRAS, INPUT_PULLUP);
  pinMode(SENSOR_IR, INPUT);
  pinMode(LED_VERMELHO, OUTPUT);
  pinMode(LED_AMARELO, OUTPUT);
  pinMode(LED_VERDE, OUTPUT);
  pinMode(BUZZER, OUTPUT);

  inicializarRelogio();
  stopMotor();
  atualizarLEDs();

  Serial.println("Sistema iniciado. Passe cartao ou digite codigo.");
}

// ================= LOOP PRINCIPAL =================
void loop() {
  atualizarRelogio();
  lerBotoesDisplay();
  char tecla = keypad.getKey();

  if (splash) {
    telaSplash();
    return;
  }

  if (emMenu && !emTopico) {
    lerEncoder();
    desenharMenu();
  }

  if (emTopico) {
    // Tratamento especial para o novo item "Inserir PIN" (índice 0)
    if (menuIndex == 0) {
      if (tecla) {
        if (tecla == '#') {
          if (codigoDigitado == codigoCorreto) {
            Serial.println("PIN correto -> Abrindo");
            startMotor(true);
            codigoDigitado = "";
            emTopico = false;           // sai da tela após abrir
          } else {
            Serial.println("PIN errado");
            // Aqui podes adicionar um beep de erro ou piscar algo se quiseres
            codigoDigitado = "";
          }
        } else if (tecla == '*') {
          codigoDigitado = "";
        } else if (codigoDigitado.length() < 8) {   // limite razoável
          codigoDigitado += tecla;
        }
      }
      desenharTelaInserirPIN();
    }
    // Outros tópicos (incluindo redefinir PIN)
    else {
      if (menuIndex == 6 && tecla) {   // Redefinir PIN
        if (aguardandoAdmin) {
          if (tecla == '#') {
            if (inputAdmin == adminPass) {
              aguardandoAdmin = false;
              aguardandoNovoPIN = true;
              inputNovoPIN = "";
              Serial.println("Admin OK - digite novo PIN");
            } else inputAdmin = "";
          } else if (tecla == '*') inputAdmin = "";
          else inputAdmin += tecla;
        } else if (aguardandoNovoPIN) {
          if (tecla == '#') {
            if (inputNovoPIN.length() > 0) {
              codigoCorreto = inputNovoPIN;
              aguardandoNovoPIN = false;
              emTopico = false;
              Serial.println("Novo PIN definido!");
            }
          } else if (tecla == '*') inputNovoPIN = "";
          else inputNovoPIN += tecla;
        }
      }
      desenharTopico();
    }
  }

  // RFID e keypad direto (fora do menu) — continua a funcionar normalmente
  if (!motorRodando && estado == PARADO && !emTopico) {
    // RFID
if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {

  Serial.print("UID do cartao: ");

  for (byte i = 0; i < rfid.uid.size; i++) {
    Serial.print(rfid.uid.uidByte[i], HEX);
    Serial.print(" ");
  }

  Serial.println();

  if (checkUID(rfid.uid.uidByte)) {
    Serial.println("CARTAO AUTORIZADO -> ABRIR PORTAO");
    startMotor(true);
  } else {
    Serial.println("CARTAO NAO AUTORIZADO");
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

    // Keypad direto (fora do menu)
    if (tecla) {
      if (tecla == '#') {
        if (codigoDigitado == codigoCorreto) {
          Serial.println("PIN correto -> Abrindo");
          startMotor(true);
        } else {
          Serial.println("PIN errado");
        }
        codigoDigitado = "";
      } else if (tecla == '*') {
        codigoDigitado = "";
      } else {
        codigoDigitado += tecla;
      }
    }
  }

  // Máquina de estados do portão
  switch (estado) {
    case ABRINDO:
      somAbrindo();
      if (digitalRead(FIM_FRENTE) == LOW) {
        stopMotor();
        noTone(BUZZER);
        estado = ABERTO;
        tempoInicio = millis();
        adicionarLog(true);
        atualizarLEDs();
        Serial.println("Portao completamente aberto -> LED VERDE");
      }
      break;

    case ABERTO:
      if (digitalRead(SENSOR_IR) == LOW) tempoInicio = millis();
      else if (millis() - tempoInicio >= tempoEspera - tempoPreAviso &&
               millis() - tempoInicio < tempoEspera) somPreAviso();
      else if (millis() - tempoInicio >= tempoEspera) {
        noTone(BUZZER);
        startMotor(false);
      }
      break;

    case FECHANDO:
      somFechando();
      if (digitalRead(FIM_TRAS) == LOW) {
        stopMotor();
        noTone(BUZZER);
        estado = PARADO;
        adicionarLog(false);
        atualizarLEDs();
        Serial.println("Portao completamente fechado -> LED VERMELHO");
      } else if (digitalRead(SENSOR_IR) == LOW) {
        stopMotor();
        sireneIR();
        startMotor(true);
      }
      break;

    case PARADO:
      noTone(BUZZER);
      break;
  }

  // Garantia extra para LEDs
  if (!motorRodando) {
    atualizarLEDs();
  }
}
