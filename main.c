/*
 * ESP32 Tic-Tac-Toe Client
 * CS2600 - Spring 2026
 *
 * Hardware:
 *   - 4x4 Keypad
 *   - 16x2 LCD via I2C (LiquidCrystal_I2C)
 *
 *   1-9  select cell:
 *     1=r1c1  2=r1c2  3=r1c3
 *     4=r2c1  5=r2c2  6=r2c3
 *     7=r3c1  8=r3c2  9=r3c3
 *   #    confirm and send move
 *   *    cancel / re-select
 *
 * ── MQTT Topics ──────────────────────────────────────────────────────────────
 *   Subscribes to:
 *     tictactoe/board          board state  e.g. "X|.|O / .|X|. / O|.|X"
 *     tictactoe/status         game prompts / results
 *     tictactoe/mode           "1player" | "2player"  (retained by broker)
 *   Publishes to:
 *     tictactoe/player1/move   (1-player mode)  "row,col"
 *     tictactoe/player2/move   (2-player mode)  "row,col"
 *
 */

#include <Keypad.h>
#include <LiquidCrystal_I2C.h>
#include <PubSubClient.h>
#include <WiFi.h>

// ─── WIFI CONFIG
// ──────────────────────────────────────────────────────────────
const char *WIFI_SSID = "WIFI_SSID";
const char *WIFI_PASSWORD = "WIFI_PASSWORD";

// ─── MQTT CONFIG
// ──────────────────────────────────────────────────────────────
const char *MQTT_BROKER = "cgulsbycs2600.duckdns.org";
const int MQTT_PORT = 1883;

const char *TOPIC_BOARD = "tictactoe/board";
const char *TOPIC_STATUS = "tictactoe/status";
const char *TOPIC_MODE = "tictactoe/mode";
const char *TOPIC_P1_MOVE = "tictactoe/player1/move";
const char *TOPIC_P2_MOVE = "tictactoe/player2/move";

// ─── LCD
// ──────────────────────────────────────────────────────────────────────
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ─── KEYPAD
// ───────────────────────────────────────────────────────────────────
const byte ROWS = 4, COLS = 4;
char keys[ROWS][COLS] = {{'1', '2', '3', 'A'},
                         {'4', '5', '6', 'B'},
                         {'7', '8', '9', 'C'},
                         {'*', '0', '#', 'D'}};
byte rowPins[ROWS] = {19, 18, 5, 14};
byte colPins[COLS] = {13, 4, 2, 15};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ─── GAME STATE
// ───────────────────────────────────────────────────────────────
WiFiClient espClient;
PubSubClient mqtt(espClient);

// gameMode: 0 = unknown (waiting for tictactoe/mode), 1 = P1 (X), 2 = P2 (O)
int gameMode = 0;
bool myTurn = false;
char pendingKey = 0;

// Deferred LCD update buffers — written in callback, consumed in loop()
bool lcdNeedsUpdate = false;
bool boardNeedsUpdate = false;
char pendingStatusBuf[128] = {0};
char pendingBoardBuf[128] = {0};

// ─── LCD HELPERS ─────────────────────────────────────────────────────────────

void lcdPrint(String line0, String line1) {
  lcd.clear();
  delay(10);
  lcd.setCursor(0, 0);
  lcd.print(line0.substring(0, 16));
  lcd.setCursor(0, 1);
  lcd.print(line1.substring(0, 16));
}

// Word-wrap a string across the two LCD lines
void lcdPrintWrapped(const char *msg) {
  String s = String(msg);
  int splitAt = 16;
  if (s.length() > 16) {
    splitAt = s.lastIndexOf(' ', 15);
    if (splitAt <= 0)
      splitAt = 16;
  }
  String line0 = s.substring(0, splitAt);
  String line1 =
      (s.length() > (unsigned int)splitAt) ? s.substring(splitAt + 1) : "";
  lcdPrint(line0, line1);
}

// Display compact board across both LCD rows
// Board string: "X|.|O / .|X|. / O|.|X"
void displayBoard(String boardStr) {
  // Collect the 9 cell characters
  String cells = "";
  for (unsigned int i = 0; i < boardStr.length(); i++) {
    char c = boardStr[i];
    if (c == 'X' || c == 'O' || c == '.')
      cells += c;
  }
  if (cells.length() != 9)
    return;

  // '.' -> ' ' for display
  String d = "";
  for (int i = 0; i < 9; i++)
    d += (cells[i] == '.') ? ' ' : cells[i];

  // LCD line 0: board rows 1 and 2  e.g. "X|O|.  .|X|."
  String line0 = "";
  line0 += d[0];
  line0 += "|";
  line0 += d[1];
  line0 += "|";
  line0 += d[2];
  line0 += "  ";
  line0 += d[3];
  line0 += "|";
  line0 += d[4];
  line0 += "|";
  line0 += d[5];

  // LCD line 1: board row 3 + whose-turn hint
  String turnHint;
  if (myTurn) {
    turnHint = "  YOUR TURN";
  } else {
    // Show which player we are waiting for
    turnHint = (gameMode == 2) ? "  P1 turn  " : "  P2 turn  ";
  }
  String line1 = "";
  line1 += d[6];
  line1 += "|";
  line1 += d[7];
  line1 += "|";
  line1 += d[8];
  line1 += turnHint;

  lcd.clear();
  delay(10);
  lcd.setCursor(0, 0);
  lcd.print(line0.substring(0, 16));
  lcd.setCursor(0, 1);
  lcd.print(line1.substring(0, 16));
}

// ─── MQTT CALLBACK ───────────────────────────────────────────────────────────
// Keep this fast — no blocking, no lcd.clear().
void mqttCallback(char *topic, byte *payload, unsigned int length) {
  char msg[128];
  unsigned int len = (length < 127) ? length : 127;
  memcpy(msg, payload, len);
  msg[len] = '\0';

  Serial.print("[MQTT] ");
  Serial.print(topic);
  Serial.print(": ");
  Serial.println(msg);

  // ── tictactoe/mode (retained — arrives right after subscribe) ──────────────
  if (strcmp(topic, TOPIC_MODE) == 0) {
    int newMode = 0;
    if (strstr(msg, "2player"))
      newMode = 2;
    else if (strstr(msg, "1player"))
      newMode = 1;

    if (newMode != 0 && newMode != gameMode) {
      gameMode = newMode;
      myTurn = false;
      pendingKey = 0;
      Serial.print("[MODE] Set to: ");
      Serial.println(gameMode == 2 ? "2-Player (ESP32 = O)"
                                   : "1-Player (ESP32 = X)");

      strncpy(pendingStatusBuf,
              gameMode == 2 ? "2P mode: ESP32=O" : "1P mode: ESP32=X", 127);
      lcdNeedsUpdate = true;
    }
    return;
  }

  // ── tictactoe/board ────────────────────────────────────────────────────────
  if (strcmp(topic, TOPIC_BOARD) == 0) {
    strncpy(pendingBoardBuf, msg, 127);
    boardNeedsUpdate = true;
    return;
  }

  // ── tictactoe/status ───────────────────────────────────────────────────────
  if (strcmp(topic, TOPIC_STATUS) == 0) {
    strncpy(pendingStatusBuf, msg, 127);
    lcdNeedsUpdate = true;

    bool isP1Prompt = (strstr(msg, "Player 1") && strstr(msg, "row,col"));
    bool isP2Prompt = (strstr(msg, "Player 2") && strstr(msg, "row,col"));
    bool isGameOver = (strstr(msg, "wins") || strstr(msg, "draw"));

    if (isGameOver) {
      myTurn = false;
      Serial.println("[GAME] Game over.");
      return;
    }

    // ESP32 acts on its own player's turn prompt only
    if (gameMode == 1 && isP1Prompt) {
      myTurn = true;
      pendingKey = 0;
      Serial.println("[GAME] Your turn! (1-player / X)");
    } else if (gameMode == 2 && isP2Prompt) {
      myTurn = true;
      pendingKey = 0;
      Serial.println("[GAME] Your turn! (2-player / O)");
    } else {
      myTurn = false;
    }
  }
}

// ─── WIFI / MQTT CONNECT
// ──────────────────────────────────────────────────────
void connectWifi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
}

void connectMqtt() {
  while (!mqtt.connected()) {
    Serial.print("Connecting to MQTT broker...");
    if (mqtt.connect("ESP32_TicTacToe")) {
      Serial.println("connected!");
      mqtt.subscribe(TOPIC_BOARD);
      mqtt.subscribe(TOPIC_STATUS);
      mqtt.subscribe(TOPIC_MODE); // retained — instantly delivers current mode
      Serial.println("Subscribed to board / status / mode.");
    } else {
      Serial.print("failed rc=");
      Serial.print(mqtt.state());
      Serial.println(" — retry in 3s");
      delay(3000);
    }
  }
}

// ─── SETUP ───────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  lcd.init();
  lcd.backlight();
  lcdPrint("TicTacToe ESP32", "Connecting...");

  connectWifi();

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  connectMqtt();

  // The retained tictactoe/mode message fires immediately after subscribe,
  // so gameMode will be set during the first mqtt.loop() call in loop().
  lcdPrint("Waiting for", "game mode...");
  Serial.println("Ready. Awaiting mode from broker...");
  Serial.println("Keys: 1-9=cell  #=send  *=cancel");
}

// ─── LOOP ────────────────────────────────────────────────────────────────────
void loop() {
  if (!mqtt.connected())
    connectMqtt();
  mqtt.loop();

  // ── Deferred LCD rendering ─────────────────────────────────────────────────
  if (myTurn) {
    // Our turn — show the status/prompt so player knows to act
    if (lcdNeedsUpdate) {
      lcdNeedsUpdate = false;
      boardNeedsUpdate = false;
      lcdPrintWrapped(pendingStatusBuf);
    }
  } else {
    // Not our turn — prefer showing the board; fall back to status text
    if (boardNeedsUpdate) {
      boardNeedsUpdate = false;
      lcdNeedsUpdate = false;
      displayBoard(String(pendingBoardBuf));
    } else if (lcdNeedsUpdate) {
      lcdNeedsUpdate = false;
      lcdPrintWrapped(pendingStatusBuf);
    }
  }

  // ── Keypad input ─────────────────────────────────────────────────────────
  if (!myTurn)
    return;

  char key = keypad.getKey();
  if (!key)
    return;

  Serial.print("[KEY] ");
  Serial.println(key);

  // * = cancel selection
  if (key == '*') {
    pendingKey = 0;
    lcd.setCursor(0, 1);
    lcd.print("Cancelled.      ");
    Serial.println("Selection cancelled.");
    return;
  }

  // # = confirm and send move
  if (key == '#') {
    if (pendingKey == 0) {
      lcd.setCursor(0, 1);
      lcd.print("Pick 1-9 first! ");
      Serial.println("No cell selected yet.");
      return;
    }

    int cell = pendingKey - '0';
    int row = ((cell - 1) / 3) + 1;
    int col = ((cell - 1) % 3) + 1;

    char moveMsg[8];
    snprintf(moveMsg, sizeof(moveMsg), "%d,%d", row, col);

    // Route to the correct MQTT topic based on our role
    const char *moveTopic = (gameMode == 2) ? TOPIC_P2_MOVE : TOPIC_P1_MOVE;

    Serial.print("[MQTT] Publishing to ");
    Serial.print(moveTopic);
    Serial.print(": ");
    Serial.println(moveMsg);

    mqtt.publish(moveTopic, moveMsg);

    lcd.setCursor(0, 1);
    String sent = "Sent: " + String(moveMsg) + "        ";
    lcd.print(sent.substring(0, 16));

    myTurn = false;
    pendingKey = 0;
    return;
  }

  // 1-9 = preview cell selection
  if (key >= '1' && key <= '9') {
    pendingKey = key;
    int cell = key - '0';
    int row = ((cell - 1) / 3) + 1;
    int col = ((cell - 1) % 3) + 1;

    Serial.print("Cell ");
    Serial.print(key);
    Serial.print(" -> R");
    Serial.print(row);
    Serial.print("C");
    Serial.println(col);

    lcd.setCursor(0, 1);
    String preview =
        "Sel:" + String(key) + " R" + row + "C" + col + " #=OK *=X";
    lcd.print(preview.substring(0, 16));
    return;
  }

  // A, B, C, D, 0 — ignore
  Serial.println("Key ignored.");
}
