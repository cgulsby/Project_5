#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#define BROKER          "cgulsbycs2600.duckdns.org"
#define TOPIC_P1_MOVE   "tictactoe/player1/move"
#define TOPIC_P2_MOVE   "tictactoe/player2/move"
#define TOPIC_BOARD     "tictactoe/board"
#define TOPIC_STATUS    "tictactoe/status"
#define TOPIC_MODE      "tictactoe/mode"

char board[3][3];
const char PLAYER_ONE = 'X';
const char PLAYER_TWO = 'O';

pid_t ai_pid = 0;

void print_board();
void publish_board();
void publish_status(const char *msg);
void publish_mode(const char *mode);
void reset_board();
int  check_free_spaces();
void player_one_move();
void player_two_move();
char check_winner();
void print_winner(char winner);
void launch_ai();
void stop_ai();
// ─────────────────────────────────────────────────────────────────────────────

int main() {

  // ── Title Screen ────────────────────────────────────────────────────────────
  printf("\n\n");
  printf("+------------------------------------------+\n");
  printf("|       Tic          Tac        ToESP32    |\n");
  printf("+------------------------------------------+\n");
  printf("\n");

  // ── Mode Selection ──────────────────────────────────────────────────────────
  int mode = 0;
  while (mode != 1 && mode != 2) {
    printf("+------------------------------------------+\n");
    printf("| Select Game Mode:                        |\n");
    printf("|   1 = 1 Player  (You vs AI)              |\n");
    printf("|   2 = 2 Players (You vs ESP32)           |\n");
    printf("+------------------------------------------+\n");
    printf("Enter 1 or 2: ");
    if (scanf("%d", &mode) != 1) {
      // flush bad input
      int c; while ((c = getchar()) != '\n' && c != EOF);
      mode = 0;
    }
    if (mode != 1 && mode != 2)
      printf("Invalid choice. Please enter 1 or 2.\n\n");
  }

  printf("\n");
  if (mode == 1) {
    printf("+------------------------------------------+\n");
    printf("| Instructions:                            |\n");
    printf("|------------------------------------------|\n");
    printf("| Player 1 (You!) is X                     |\n");
    printf("| Player 2 (AI)   is O                     |\n");
    printf("+------------------------------------------+\n\n\n");
  } else {
    printf("+------------------------------------------+\n");
    printf("| Instructions:                            |\n");
    printf("|------------------------------------------|\n");
    printf("| Player 1 (You!) is X                     |\n");
    printf("| Player 2 (ESP32) is O                    |\n");
    printf("+------------------------------------------+\n\n\n");
  }

  // Broadcast mode so GUI and ESP32 can react
  if (mode == 1)
    publish_mode("1player");
  else
    publish_mode("2player");

  // In 1-player mode, spawn the AI script as a background process
  if (mode == 1)
    launch_ai();

  char winner = ' ';
  reset_board();

  while (winner == ' ' && check_free_spaces() != 0) {
    print_board();
    publish_board();

    player_one_move();
    winner = check_winner();
    if (winner != ' ' || check_free_spaces() == 0)
      break;

    printf("\n\n");
    print_board();
    publish_board();

    player_two_move();
    winner = check_winner();
    if (winner != ' ' || check_free_spaces() == 0)
      break;
  }

  print_board();
  publish_board();
  print_winner(winner);

  // Publish final result
  if (winner == PLAYER_ONE)
    publish_status("Player 1 (X) wins! Thanks for playing!");
  else if (winner == PLAYER_TWO)
    publish_status("Player 2 (O) wins! Thanks for playing!");
  else
    publish_status("It's a draw! Thanks for playing!");

  // Clean up AI subprocess if running
  if (mode == 1)
    stop_ai();

  return 0;
}

// launch AI script
// Forks and execs ai_player.sh in the background so it can respond
// to MQTT prompts independently. The server game loop continues normally —
// player_two_move() still blocks on MQTT for a "row,col" from any source,
// which the AI script provides automatically.
void launch_ai() {
  printf("[AI] Launching ai_player.sh...\n");
  ai_pid = fork();
  if (ai_pid == 0) {
    // Child: exec the script. Use execl so the child replaces itself.
    execl("/bin/bash", "bash", "./ai_player.sh", (char *)NULL);
    perror("[AI] execl failed");
    exit(1);
  } else if (ai_pid < 0) {
    perror("[AI] fork failed");
    ai_pid = 0;
  } else {
    printf("[AI] ai_player.sh started (pid %d)\n", ai_pid);
  }
}

// stop AI script
void stop_ai() {
  if (ai_pid > 0) {
    kill(ai_pid, SIGTERM);
    waitpid(ai_pid, NULL, 0);
    printf("[AI] ai_player.sh stopped.\n");
    ai_pid = 0;
  }
}

// Broadcasts "1player" or "2player" so the GUI and ESP32 know what to do.
void publish_mode(const char *mode) {
  char cmd[256];
  snprintf(cmd, sizeof(cmd),
           "mosquitto_pub -h %s -t %s -m \"%s\" -r",
           BROKER, TOPIC_MODE, mode);
  system(cmd);
  printf("[MQTT] Mode published: %s\n", mode);
}

// print board to terminal
void print_board() {
  printf("    1   2   3\n");
  printf("  +-----------+\n");
  printf("1 | %c | %c | %c |\n", board[0][0], board[0][1], board[0][2]);
  printf("  |---|---|---|\n");
  printf("2 | %c | %c | %c |\n", board[1][0], board[1][1], board[1][2]);
  printf("  |---|---|---|\n");
  printf("3 | %c | %c | %c |\n", board[2][0], board[2][1], board[2][2]);
  printf("  +-----------+\n");
}

// publish board via MQTT
void publish_board() {
  char msg[64];
  char b[3][3];
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      b[i][j] = (board[i][j] == ' ') ? '.' : board[i][j];

  snprintf(msg, sizeof(msg), "%c|%c|%c / %c|%c|%c / %c|%c|%c",
           b[0][0], b[0][1], b[0][2],
           b[1][0], b[1][1], b[1][2],
           b[2][0], b[2][1], b[2][2]);

  char cmd[256];
  snprintf(cmd, sizeof(cmd),
           "mosquitto_pub -h %s -t %s -m \"%s\"",
           BROKER, TOPIC_BOARD, msg);
  system(cmd);
  printf("[MQTT] Board published: %s\n", msg);
}

// publish status
void publish_status(const char *msg) {
  char cmd[256];
  snprintf(cmd, sizeof(cmd),
           "mosquitto_pub -h %s -t %s -m \"%s\"",
           BROKER, TOPIC_STATUS, msg);
  system(cmd);
  printf("[MQTT] Status published: %s\n", msg);
}

// reset board
void reset_board() {
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      board[i][j] = ' ';
}

// count free spaces
int check_free_spaces() {
  int free_spaces = 9;
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      if (board[i][j] != ' ')
        free_spaces--;
  return free_spaces;
}

int get_mqtt_move(const char *topic, int *x, int *y) {
  char cmd[256];
  snprintf(cmd, sizeof(cmd),
           "mosquitto_sub -h %s -t %s -C 1",
           BROKER, topic);

  FILE *fp = popen(cmd, "r");
  if (!fp) {
    printf("[MQTT] ERROR: popen failed\n");
    return 0;
  }

  char result[64] = {0};
  fgets(result, sizeof(result), fp);
  pclose(fp);

  printf("[MQTT] Received: %s", result);

  int r, c;
  if (sscanf(result, "%d,%d", &r, &c) != 2) {
    printf("[MQTT] ERROR: bad payload format (expected \"row,col\")\n");
    return 0;
  }

  *x = r - 1;  // convert to 0-indexed
  *y = c - 1;
  return 1;
}

// Player 1 move
void player_one_move() {
  int x, y;

  publish_status("Player 1 (X): send row,col to tictactoe/player1/move");
  printf("\n\nWaiting for Player 1 move via MQTT...\n");

  do {
    if (!get_mqtt_move(TOPIC_P1_MOVE, &x, &y)) {
      publish_status("Bad format. Send: row,col (e.g. 2,3)");
      continue;
    }

    if (x < 0 || x > 2 || y < 0 || y > 2) {
      printf("Move out of bounds, try again\n");
      publish_status("Out of bounds! Row and col must be 1-3");
      continue;
    }

    if (board[x][y] != ' ') {
      printf("Space taken, try again\n");
      publish_status("Space taken! Choose a free space");
      continue;
    }

    board[x][y] = PLAYER_ONE;
    printf("Player 1 played at row %d, col %d\n", x + 1, y + 1);
    break;

  } while (1);
}

// ─── PLAYER 2 MOVE (AI script or ESP32 via MQTT) ─────────────────────────────
// The move source depends on which mode was selected at startup:
//   1-player — ai_player.sh publishes to TOPIC_P2_MOVE automatically
//   2-player — the ESP32 publishes to TOPIC_P2_MOVE
// Either way, this function just waits for the correct MQTT payload.
void player_two_move() {
  int x, y;

  printf("\n\nWaiting for Player 2 move via MQTT...\n");

  do {
    // Open the subscriber FIRST so no message can slip through the gap
    // between publishing the prompt and starting to listen.
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "mosquitto_sub -h %s -t %s -C 1", BROKER, TOPIC_P2_MOVE);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
      printf("[MQTT] ERROR: popen failed\n");
      continue;
    }

    // Small delay to ensure mosquitto_sub has connected to the broker
    // before we publish the prompt that triggers the AI / ESP32 to send
    usleep(300000); // 300 ms

    // NOW publish the turn prompt
    publish_status("Player 2 (O): send row,col to tictactoe/player2/move");

    char result[64] = {0};
    fgets(result, sizeof(result), fp);
    pclose(fp);

    printf("[MQTT] Received: %s", result);

    int r, c;
    if (sscanf(result, "%d,%d", &r, &c) != 2) {
      publish_status("Bad format. Send: row,col (e.g. 2,3)");
      continue;
    }

    x = r - 1;
    y = c - 1;

    if (x < 0 || x > 2 || y < 0 || y > 2) {
      printf("Move out of bounds, try again\n");
      publish_status("Out of bounds! Row and col must be 1-3");
      continue;
    }

    if (board[x][y] != ' ') {
      printf("Space taken, try again\n");
      publish_status("Space taken! Choose a free space");
      continue;
    }

    board[x][y] = PLAYER_TWO;
    printf("Player 2 played at row %d, col %d\n", x + 1, y + 1);
    publish_status("Player 2 move accepted");
    break;

  } while (1);
}

// check winner
char check_winner() {
  for (int i = 0; i < 3; i++) {
    if (board[i][0] == board[i][1] && board[i][0] == board[i][2] && board[i][0] != ' ')
      return board[i][0];
  }
  for (int i = 0; i < 3; i++) {
    if (board[0][i] == board[1][i] && board[0][i] == board[2][i] && board[0][i] != ' ')
      return board[0][i];
  }
  if (board[0][0] == board[1][1] && board[0][0] == board[2][2] && board[0][0] != ' ')
    return board[0][0];
  if (board[0][2] == board[1][1] && board[0][2] == board[2][0] && board[0][2] != ' ')
    return board[0][2];

  return ' ';
}

// Print winner
void print_winner(char winner) {
  printf("\n\n");
  if (winner == PLAYER_ONE)
    printf("Player 1 (X) is the winner!!\n");
  else if (winner == PLAYER_TWO)
    printf("Player 2 (O) is the winner!!\n");
  else
    printf("It's a draw!!\n");
  printf("Thanks for playing!\n");
}

