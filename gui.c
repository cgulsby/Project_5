/*
 * Build:
 *   sudo apt install libraylib-dev -y
 *   gcc gui.c -o gui -lraylib -lm -lpthread
 *
 * Run:
 *   ./gui
 */

#include "raylib.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BROKER "cgulsbycs2600.duckdns.org"
#define TOPIC_BOARD "tictactoe/board"
#define TOPIC_STATUS "tictactoe/status"
#define TOPIC_MOVE "tictactoe/player1/move"
#define TOPIC_MODE "tictactoe/mode"

#define SCREEN_W 600
#define SCREEN_H 700
#define BOARD_X 75
#define BOARD_Y 150
#define BOARD_SIZE 450
#define CELL_SIZE 150

#define COL_BG (Color){15, 15, 20, 255}
#define COL_GRID (Color){50, 50, 65, 255}
#define COL_X (Color){255, 80, 80, 255}
#define COL_O (Color){80, 180, 255, 255}
#define COL_HOVER (Color){255, 255, 255, 20}
#define COL_STATUS (Color){200, 200, 210, 255}
#define COL_TITLE (Color){255, 255, 255, 255}
#define COL_WIN_LINE (Color){255, 220, 50, 255}
#define COL_BTN (Color){40, 40, 60, 255}
#define COL_BTN_HOV (Color){70, 70, 100, 255}
#define COL_BTN_BDR (Color){100, 100, 140, 255}

typedef enum { SCREEN_MODE_SELECT, SCREEN_GAME } AppScreen;

char board[3][3];
char statusMsg[256];
bool myTurn = false;
int gameMode = 0; // 1 = vs AI, 2 = vs ESP32

_Atomic bool gameOver = false;
_Atomic bool listenerRun = false; // controls mqtt_listener lifecycle

pthread_mutex_t stateMutex = PTHREAD_MUTEX_INITIALIZER;

void parse_board(const char *boardStr) {
  char cells[10] = {0};
  int idx = 0;
  for (int i = 0; boardStr[i] && idx < 9; i++) {
    char c = boardStr[i];
    if (c == 'X' || c == 'O' || c == '.') {
      cells[idx++] = c;
    }
  }
  if (idx != 9)
    return;

  pthread_mutex_lock(&stateMutex);
  for (int i = 0; i < 9; i++)
    board[i / 3][i % 3] = (cells[i] == '.') ? ' ' : cells[i];
  pthread_mutex_unlock(&stateMutex);
}

void *mqtt_listener(void *arg) {
  FILE *fp = popen("mosquitto_sub -h " BROKER " -t " TOPIC_BOARD
                   " -t " TOPIC_STATUS " -v",
                   "r");
  if (!fp)
    return NULL;

  char line[512] = {0};
  while (listenerRun && fgets(line, sizeof(line), fp)) {
    line[strcspn(line, "\n")] = 0;

    char *space = strchr(line, ' ');
    if (!space)
      continue;
    *space = 0;
    char *topic = line;
    char *payload = space + 1;

    if (strcmp(topic, TOPIC_BOARD) == 0) {
      parse_board(payload);
    } else if (strcmp(topic, TOPIC_STATUS) == 0) {
      pthread_mutex_lock(&stateMutex);
      strncpy(statusMsg, payload, 255);

      if (strstr(payload, "Player 1") && strstr(payload, "row,col")) {
        myTurn = true;
        gameOver = false;
      } else if (strstr(payload, "wins") || strstr(payload, "draw")) {
        myTurn = false;
        gameOver = true;
      } else {
        myTurn = false;
      }
      pthread_mutex_unlock(&stateMutex);
    }
  }

  pclose(fp);
  return NULL;
}

typedef struct {
  int row;
  int col;
} MoveArgs;

void *publish_thread(void *arg) {
  MoveArgs *m = (MoveArgs *)arg;
  char cmd[256];
  snprintf(cmd, sizeof(cmd), "mosquitto_pub -h %s -t %s -m \"%d,%d\"", BROKER,
           TOPIC_MOVE, m->row + 1, m->col + 1);
  system(cmd);
  free(m);
  return NULL;
}

void publish_move(int row, int col) {
  MoveArgs *args = malloc(sizeof(MoveArgs));
  args->row = row;
  args->col = col;
  pthread_t t;
  pthread_create(&t, NULL, publish_thread, args);
  pthread_detach(t);
}

void *publish_mode_thread(void *arg) {
  char *mode = (char *)arg;
  char cmd[256];
  snprintf(cmd, sizeof(cmd), "mosquitto_pub -h %s -t %s -m \"%s\" -r", BROKER,
           TOPIC_MODE, mode);
  system(cmd);
  free(mode);
  return NULL;
}

void publish_mode(const char *mode) {
  char *copy = malloc(32);
  strncpy(copy, mode, 31);
  pthread_t t;
  pthread_create(&t, NULL, publish_mode_thread, copy);
  pthread_detach(t);
}

void draw_x(int cx, int cy, int size, Color color) {
  int half = size / 2 - 20;
  DrawLineEx((Vector2){cx - half, cy - half}, (Vector2){cx + half, cy + half},
             6, color);
  DrawLineEx((Vector2){cx + half, cy - half}, (Vector2){cx - half, cy + half},
             6, color);
}

void draw_o(int cx, int cy, int size, Color color) {
  DrawRing((Vector2){cx, cy}, size / 2 - 35, size / 2 - 20, 0, 360, 32, color);
}

bool draw_button(int x, int y, int w, int h, const char *label, int fontSize) {
  Vector2 mouse = GetMousePosition();
  bool hovered =
      (mouse.x >= x && mouse.x <= x + w && mouse.y >= y && mouse.y <= y + h);
  bool clicked = hovered && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);

  Color bg = hovered ? COL_BTN_HOV : COL_BTN;
  DrawRectangle(x, y, w, h, bg);
  DrawRectangleLines(x, y, w, h, COL_BTN_BDR);

  int tw = MeasureText(label, fontSize);
  DrawText(label, x + (w - tw) / 2, y + (h - fontSize) / 2, fontSize,
           hovered ? COL_TITLE : COL_STATUS);
  return clicked;
}

int main(void) {
  // Init raylib
  InitWindow(SCREEN_W, SCREEN_H, "TicTacToe — Player 1 (X)");
  SetTargetFPS(60);

  AppScreen screen = SCREEN_MODE_SELECT;
  pthread_t listenerThread;
  int hoveredCell = -1;

  while (!WindowShouldClose() && screen == SCREEN_MODE_SELECT) {
    BeginDrawing();
    ClearBackground(COL_BG);

    DrawText("TIC-TAC-TOE", SCREEN_W / 2 - MeasureText("TIC-TAC-TOE", 40) / 2,
             60, 40, COL_TITLE);
    DrawText("Select Game Mode",
             SCREEN_W / 2 - MeasureText("Select Game Mode", 20) / 2, 120, 20,
             COL_STATUS);

    int bw = 320, bh = 70, bx = (SCREEN_W - bw) / 2;

    if (draw_button(bx, 220, bw, bh, "1 Player  —  vs AI", 22)) {
      gameMode = 1;
      screen = SCREEN_GAME;
    }
    DrawText("You (X) vs the AI script (O)",
             SCREEN_W / 2 - MeasureText("You (X) vs the AI script (O)", 14) / 2,
             300, 14, COL_STATUS);

    if (draw_button(bx, 340, bw, bh, "2 Players  —  vs ESP32", 22)) {
      gameMode = 2;
      screen = SCREEN_GAME;
    }
    DrawText("You (X) vs the ESP32 (O)",
             SCREEN_W / 2 - MeasureText("You (X) vs the ESP32 (O)", 14) / 2,
             420, 14, COL_STATUS);

    EndDrawing();
  }

  if (WindowShouldClose()) {
    CloseWindow();
    return 0;
  }

  memset(board, ' ', sizeof(board));
  strncpy(statusMsg, "Waiting for game to start...", 255);

  publish_mode(gameMode == 1 ? "1player" : "2player");

  listenerRun = true;
  pthread_create(&listenerThread, NULL, mqtt_listener, NULL);

  const char *modeLabel = (gameMode == 1) ? "vs AI" : "vs ESP32";
  char title[64];
  snprintf(title, sizeof(title), "TicTacToe — Player 1 (X)  [%s]", modeLabel);
  SetWindowTitle(title);

  while (!WindowShouldClose()) {

    int mouseX = GetMouseX();
    int mouseY = GetMouseY();
    hoveredCell = -1;

    pthread_mutex_lock(&stateMutex);
    bool turn = myTurn;
    pthread_mutex_unlock(&stateMutex);

    if (turn && mouseX >= BOARD_X && mouseX <= BOARD_X + BOARD_SIZE &&
        mouseY >= BOARD_Y && mouseY <= BOARD_Y + BOARD_SIZE) {
      int col = (mouseX - BOARD_X) / CELL_SIZE;
      int row = (mouseY - BOARD_Y) / CELL_SIZE;
      int cell = row * 3 + col;
      if (col >= 0 && col < 3 && row >= 0 && row < 3) {
        pthread_mutex_lock(&stateMutex);
        bool cellFree = (board[row][col] == ' ');
        pthread_mutex_unlock(&stateMutex);
        if (cellFree)
          hoveredCell = cell;
      }
    }

    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && hoveredCell >= 0 && turn) {
      int row = hoveredCell / 3;
      int col = hoveredCell % 3;
      pthread_mutex_lock(&stateMutex);
      myTurn = false;
      pthread_mutex_unlock(&stateMutex);
      publish_move(row, col);
    }

    BeginDrawing();
    ClearBackground(COL_BG);

    DrawText("TIC-TAC-TOE", SCREEN_W / 2 - MeasureText("TIC-TAC-TOE", 36) / 2,
             30, 36, COL_TITLE);

    const char *subLabel = (gameMode == 1)
                               ? "You are Player 1  (X)  —  vs AI"
                               : "You are Player 1  (X)  —  vs ESP32";
    DrawText(subLabel, SCREEN_W / 2 - MeasureText(subLabel, 16) / 2, 78, 16,
             COL_X);

    const char *turnText = turn ? "YOUR TURN — click a cell" : "Waiting...";
    Color turnColor = turn ? COL_X : COL_STATUS;
    DrawText(turnText, SCREEN_W / 2 - MeasureText(turnText, 16) / 2, 104, 16,
             turnColor);

    DrawRectangle(BOARD_X, BOARD_Y, BOARD_SIZE, BOARD_SIZE,
                  (Color){25, 25, 35, 255});

    if (hoveredCell >= 0) {
      int hr = hoveredCell / 3;
      int hc = hoveredCell % 3;
      DrawRectangle(BOARD_X + hc * CELL_SIZE, BOARD_Y + hr * CELL_SIZE,
                    CELL_SIZE, CELL_SIZE, COL_HOVER);
    }

    for (int i = 1; i < 3; i++) {
      DrawLineEx((Vector2){BOARD_X + i * CELL_SIZE, BOARD_Y},
                 (Vector2){BOARD_X + i * CELL_SIZE, BOARD_Y + BOARD_SIZE}, 3,
                 COL_GRID);
      DrawLineEx((Vector2){BOARD_X, BOARD_Y + i * CELL_SIZE},
                 (Vector2){BOARD_X + BOARD_SIZE, BOARD_Y + i * CELL_SIZE}, 3,
                 COL_GRID);
    }

    // Pieces
    pthread_mutex_lock(&stateMutex);
    for (int r = 0; r < 3; r++) {
      for (int c = 0; c < 3; c++) {
        int cx = BOARD_X + c * CELL_SIZE + CELL_SIZE / 2;
        int cy = BOARD_Y + r * CELL_SIZE + CELL_SIZE / 2;
        if (board[r][c] == 'X')
          draw_x(cx, cy, CELL_SIZE, COL_X);
        else if (board[r][c] == 'O')
          draw_o(cx, cy, CELL_SIZE, COL_O);
      }
    }
    pthread_mutex_unlock(&stateMutex);

    DrawRectangleLines(BOARD_X, BOARD_Y, BOARD_SIZE, BOARD_SIZE, COL_GRID);

    pthread_mutex_lock(&stateMutex);
    char statusCopy[256];
    strncpy(statusCopy, statusMsg, 255);
    pthread_mutex_unlock(&stateMutex);

    DrawText(statusCopy, SCREEN_W / 2 - MeasureText(statusCopy, 16) / 2,
             BOARD_Y + BOARD_SIZE + 20, 16, COL_STATUS);

    if (gameOver) {
      DrawRectangle(0, 0, SCREEN_W, SCREEN_H, (Color){0, 0, 0, 150});
      DrawText(statusCopy, SCREEN_W / 2 - MeasureText(statusCopy, 30) / 2,
               SCREEN_H / 2 - 15, 30, COL_WIN_LINE);
      DrawText("Close window to exit",
               SCREEN_W / 2 - MeasureText("Close window to exit", 16) / 2,
               SCREEN_H / 2 + 30, 16, COL_STATUS);
    }

    EndDrawing();
  }

  listenerRun = false;
  gameOver = true;
  pthread_join(listenerThread, NULL);
  CloseWindow();
  return 0;
}
