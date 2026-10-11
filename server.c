#include <arpa/inet.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PORT 8080

typedef struct {
  // Game state
  int h_lines[2][3];
  int v_lines[3][2];
  char boxes[2][2];
  int scores[3];

  // Player info
  char names[3][50];
  char symbols[3];
  int client_sockets[3];
  int active[3];

  // Turn management
  int current_player;
  int game_over;
  int game_count;

  // Persistence
  int total_wins[3];

  // Process-shared mutexes
  pthread_mutex_t game_mutex;
  pthread_mutex_t log_mutex;
  pthread_mutex_t score_mutex;

  // Logger queue
  char log_queue[100][256];
  int log_head;
  int log_tail;
  sem_t log_sem;

} SharedGameState;

/* ----------------------------- Utilities ----------------------------- */

void init_game(SharedGameState *game) {
  // Clear horizontal lines
  for (int r = 0; r < 2; r++)
    for (int c = 0; c < 3; c++)
      game->h_lines[r][c] = 0;

  // Clear vertical lines
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 2; c++)
      game->v_lines[r][c] = 0;

  // Clear boxes
  for (int r = 0; r < 2; r++)
    for (int c = 0; c < 2; c++)
      game->boxes[r][c] = ' ';

  // Clear scores
  for (int i = 0; i < 3; i++)
    game->scores[i] = 0;
}

void broadcast_board(SharedGameState *game) {
  char msg[256];
  int offset = sprintf(msg, "BOARD:");

  // Horizontal lines (2×3 = 6 bytes)
  for (int r = 0; r < 2; r++)
    for (int c = 0; c < 3; c++)
      msg[offset++] = game->h_lines[r][c] ? '1' : '0';

  // Vertical lines (3×2 = 6 bytes)
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 2; c++)
      msg[offset++] = game->v_lines[r][c] ? '1' : '0';

  // Boxes (2×2 = 4 bytes)
  for (int r = 0; r < 2; r++)
    for (int c = 0; c < 2; c++)
      msg[offset++] = game->boxes[r][c];

  // Scores (3 integers)
  offset += sprintf(msg + offset, ":%d,%d,%d\n", game->scores[0],
                    game->scores[1], game->scores[2]);

  // Send to all 3 clients
  for (int i = 0; i < 3; i++)
    send(game->client_sockets[i], msg, strlen(msg), 0);
}

void send_prompt(SharedGameState *game) {
  int p = game->current_player;

  send(game->client_sockets[p], "PROMPT\n", 7, 0);

  // Send WAIT to the other 2 players
  for (int i = 0; i < 3; i++) {
    if (i != p)
      send(game->client_sockets[i], "WAIT\n", 5, 0);
  }
}

/* ----------------------------- Line Number Conversion ----------------------------- */

// Convert line number (1-12) to line type and coordinates
// Lines 1-6: Horizontal (h_lines[0-1][0-2])
// Lines 7-12: Vertical (v_lines[0-2][0-1])
// Returns: 0 if invalid, 1 if valid horizontal, 2 if valid vertical
int line_number_to_coords(int line_num, int *r, int *c) {
  if (line_num < 1 || line_num > 12)
    return 0;

  if (line_num <= 6) {
    // Horizontal lines (1-6)
    *r = (line_num - 1) / 3;
    *c = (line_num - 1) % 3;
    return 1;
  } else {
    // Vertical lines (7-12)
    int v_idx = line_num - 7;
    *r = v_idx / 2;
    *c = v_idx % 2;
    return 2;
  }
}

/* ----------------------------- Win/Draw Check ----------------------------- */

int check_and_score_boxes(SharedGameState *game, int player) {
  int scored = 0;
  char symbol = game->symbols[player];

  for (int r = 0; r < 2; r++) {
    for (int c = 0; c < 2; c++) {
      // Skip if box already owned
      if (game->boxes[r][c] != ' ')
        continue;

      // Check if all 4 sides are drawn
      int top = game->h_lines[r][c];
      int bottom = game->h_lines[r + 1][c];
      int left = game->v_lines[r][c];
      int right = game->v_lines[r][c + 1];

      if (top && bottom && left && right) {
        game->boxes[r][c] = symbol;
        game->scores[player]++;
        scored++;
      }
    }
  }

  return scored;
}

int check_game_over(SharedGameState *game) {
  // Game ends when all 4 boxes are filled
  for (int r = 0; r < 2; r++)
    for (int c = 0; c < 2; c++)
      if (game->boxes[r][c] == ' ')
        return 0;
  return 1;
}

int find_winner(SharedGameState *game) {
  // Find highest score
  int max_score = game->scores[0];
  for (int i = 1; i < 3; i++)
    if (game->scores[i] > max_score)
      max_score = game->scores[i];

  // Count how many players have max score
  int winners = 0;
  int winner_idx = -1;
  for (int i = 0; i < 3; i++) {
    if (game->scores[i] == max_score) {
      winners++;
      winner_idx = i;
    }
  }

  // Return -1 for draw (2+ players tied), else winner index
  return (winners > 1) ? -1 : winner_idx;
}

/* ----------------------------- Shared Memory Setup ----------------------------- */

SharedGameState *setup_shared_memory() {
  // Create shared memory segment
  int shm_fd = shm_open("/dotsboxes_game", O_CREAT | O_RDWR, 0666);
  if (shm_fd < 0) {
    perror("shm_open");
    exit(1);
  }

  // Set size
  if (ftruncate(shm_fd, sizeof(SharedGameState)) < 0) {
    perror("ftruncate");
    exit(1);
  }

  // Map to memory
  SharedGameState *shared = mmap(NULL, sizeof(SharedGameState),
                                 PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
  if (shared == MAP_FAILED) {
    perror("mmap");
    exit(1);
  }

  // Initialize process-shared mutexes
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);

  pthread_mutex_init(&shared->game_mutex, &attr);
  pthread_mutex_init(&shared->log_mutex, &attr);
  pthread_mutex_init(&shared->score_mutex, &attr);

  // Initialize semaphore (process-shared)
  sem_init(&shared->log_sem, 1, 0);

  // Initialize logger queue
  shared->log_head = 0;
  shared->log_tail = 0;
  
  // Initialize game count
  shared->game_count = 0;

  return shared;
}

/* ----------------------------- Logger Thread ----------------------------- */

void *logger_thread(void *arg) {
  SharedGameState *shared = (SharedGameState *)arg;
  FILE *log_file = fopen("game.log", "a");

  if (!log_file) {
    perror("Failed to open game.log");
    return NULL;
  }

  while (1) {
    // Wait for log messages
    sem_wait(&shared->log_sem);

    // Lock queue
    pthread_mutex_lock(&shared->log_mutex);

    // Process messages
    while (shared->log_tail != shared->log_head) {
      fprintf(log_file, "%s\n", shared->log_queue[shared->log_tail]);
      fflush(log_file);
      shared->log_tail = (shared->log_tail + 1) % 100;
    }

    pthread_mutex_unlock(&shared->log_mutex);
  }

  fclose(log_file);
  return NULL;
}

void add_log_message(SharedGameState *shared, const char *format, ...) {
  char timestamp[20];
  time_t now = time(NULL);
  strftime(timestamp, 20, "%Y-%m-%d %H:%M:%S", localtime(&now));

  pthread_mutex_lock(&shared->log_mutex);

  int next = (shared->log_head + 1) % 100;
  if (next == shared->log_tail) {
    // Queue full - skip
    pthread_mutex_unlock(&shared->log_mutex);
    return;
  }

  // Format message with timestamp
  snprintf(shared->log_queue[shared->log_head], 256, "[%s] ", timestamp);

  va_list args;
  va_start(args, format);
  vsnprintf(shared->log_queue[shared->log_head] + strlen(timestamp) + 3,
            256 - strlen(timestamp) - 3, format, args);
  va_end(args);

  shared->log_head = next;

  pthread_mutex_unlock(&shared->log_mutex);
  sem_post(&shared->log_sem); // Wake logger
}

/* ----------------------------- Scheduler Thread ----------------------------- */

void *scheduler_thread(void *arg) {
  SharedGameState *shared = (SharedGameState *)arg;

  while (1) {
    pthread_mutex_lock(&shared->game_mutex);

    if (!shared->game_over) {
      // Skip inactive players
      int attempts = 0;
      while (!shared->active[shared->current_player] && attempts < 3) {
        shared->current_player = (shared->current_player + 1) % 3;
        attempts++;
      }

      if (attempts >= 3) {
        shared->game_over = 1; // All disconnected
      }
    }

    pthread_mutex_unlock(&shared->game_mutex);
    usleep(50000); // 50ms
  }

  return NULL;
}

/* ----------------------------- Persistence ----------------------------- */

void load_scores(SharedGameState *shared) {
  FILE *file = fopen("scores.txt", "r");

  if (!file) {
    printf("scores.txt not found - creating new\n");
    for (int i = 0; i < 3; i++) {
      shared->total_wins[i] = 0;
    }
    return;
  }

  char line[100];
  int idx = 0;
  while (fgets(line, sizeof(line), file) && idx < 3) {
    char name[50];
    int wins;
    if (sscanf(line, "%[^:]:%d", name, &wins) == 2) {
      strcpy(shared->names[idx], name);
      shared->total_wins[idx] = wins;
      idx++;
    }
  }

  fclose(file);
}

void save_scores(SharedGameState *shared) {
  pthread_mutex_lock(&shared->score_mutex);

  FILE *file = fopen("scores.txt", "w");
  if (file) {
    for (int i = 0; i < 3; i++) {
      fprintf(file, "%s:%d\n", shared->names[i], shared->total_wins[i]);
    }
    fclose(file);
  }

  pthread_mutex_unlock(&shared->score_mutex);
}

void update_winner_score(SharedGameState *shared, int winner_idx) {
  pthread_mutex_lock(&shared->score_mutex);
  shared->total_wins[winner_idx]++;
  pthread_mutex_unlock(&shared->score_mutex);

  save_scores(shared);
  add_log_message(shared, "Winner: %s - Total wins: %d",
                  shared->names[winner_idx], shared->total_wins[winner_idx]);
}

/* ----------------------------- Signal Handlers ----------------------------- */

void sigchld_handler(int sig) {
  (void)sig; // Suppress unused parameter warning
  // Reap all zombie children
  while (waitpid(-1, NULL, WNOHANG) > 0)
    ;
}

SharedGameState *global_shared = NULL; // For signal handler

void sigint_handler(int sig) {
  (void)sig; // Suppress unused parameter warning
  printf("\nShutting down server...\n");

  if (global_shared) {
    save_scores(global_shared);
    munmap(global_shared, sizeof(SharedGameState));
    shm_unlink("/dotsboxes_game");
  }

  exit(0);
}

/* ----------------------------- Client Handler (Child Process) ----------------------------- */

void handle_client(SharedGameState *shared, int player_idx) {
  int sock = shared->client_sockets[player_idx];

  while (1) {
    // Check if it's my turn
    pthread_mutex_lock(&shared->game_mutex);
    int my_turn = (shared->current_player == player_idx && !shared->game_over);
    int game_ended = shared->game_over;
    pthread_mutex_unlock(&shared->game_mutex);

    if (game_ended) {
      // Wait for new game
      sleep(1);
      continue;
    }

    if (!my_turn) {
      usleep(100000); // 100ms
      continue;
    }

    // It's my turn - read move
    char buffer[256] = {0};
    int val = read(sock, buffer, 256);

    if (val <= 0) {
      pthread_mutex_lock(&shared->game_mutex);
      shared->active[player_idx] = 0;
      pthread_mutex_unlock(&shared->game_mutex);
      add_log_message(shared, "Player %s disconnected",
                      shared->names[player_idx]);
      break;
    }

    // Parse move
    if (strncmp(buffer, "MOVE:", 5) == 0 && buffer[5] >= '0' &&
        buffer[5] <= '9') {
      int line_num = atoi(buffer + 5);
      int r, c;
      int line_type = line_number_to_coords(line_num, &r, &c);

      if (line_type == 0) {
        send(sock, "INVALID\n", 8, 0);
        continue;
      }

      // Lock and process move
      pthread_mutex_lock(&shared->game_mutex);

      // Process horizontal line
      if (line_type == 1) {
        if (shared->h_lines[r][c]) {
          pthread_mutex_unlock(&shared->game_mutex);
          send(sock, "INVALID\n", 8, 0);
          continue;
        }

        shared->h_lines[r][c] = 1;
        check_and_score_boxes(shared, player_idx);
        add_log_message(shared, "Player %s drew horizontal line %d",
                        shared->names[player_idx], line_num);

        if (check_game_over(shared)) {
          int w = find_winner(shared);

          if (w != -1) {
            char msg[64];
            sprintf(msg, "WINNER:%s\n", shared->names[w]);
            for (int i = 0; i < 3; i++)
              send(shared->client_sockets[i], msg, strlen(msg), 0);

            add_log_message(shared, "Game ended - Winner: %s",
                            shared->names[w]);
            update_winner_score(shared, w);
          } else {
            for (int i = 0; i < 3; i++)
              send(shared->client_sockets[i], "DRAW\n", 5, 0);

            add_log_message(shared, "Game ended - Draw");
          }

          shared->game_over = 1;
        } else {
          shared->current_player = (shared->current_player + 1) % 3;
        }
      }
      // Process vertical line
      else if (line_type == 2) {
        if (shared->v_lines[r][c]) {
          pthread_mutex_unlock(&shared->game_mutex);
          send(sock, "INVALID\n", 8, 0);
          continue;
        }

        shared->v_lines[r][c] = 1;
        check_and_score_boxes(shared, player_idx);
        add_log_message(shared, "Player %s drew vertical line %d",
                        shared->names[player_idx], line_num);

        if (check_game_over(shared)) {
          int w = find_winner(shared);

          if (w != -1) {
            char msg[64];
            sprintf(msg, "WINNER:%s\n", shared->names[w]);
            for (int i = 0; i < 3; i++)
              send(shared->client_sockets[i], msg, strlen(msg), 0);

            add_log_message(shared, "Game ended - Winner: %s",
                            shared->names[w]);
            update_winner_score(shared, w);
          } else {
            for (int i = 0; i < 3; i++)
              send(shared->client_sockets[i], "DRAW\n", 5, 0);

            add_log_message(shared, "Game ended - Draw");
          }

          shared->game_over = 1;
        } else {
          shared->current_player = (shared->current_player + 1) % 3;
        }
      }

      pthread_mutex_unlock(&shared->game_mutex);
      broadcast_board(shared);
      send_prompt(shared);
    }
    // Backward compatibility: MOVE:H r c
    else if (strncmp(buffer, "MOVE:H ", 7) == 0) {
      int r, c;
      if (sscanf(buffer + 7, "%d %d", &r, &c) != 2) {
        send(sock, "INVALID\n", 8, 0);
        continue;
      }

      pthread_mutex_lock(&shared->game_mutex);

      if (r < 0 || r > 1 || c < 0 || c > 2 || shared->h_lines[r][c]) {
        pthread_mutex_unlock(&shared->game_mutex);
        send(sock, "INVALID\n", 8, 0);
        continue;
      }

      shared->h_lines[r][c] = 1;
      check_and_score_boxes(shared, player_idx);
      add_log_message(shared, "Player %s drew horizontal line at (%d,%d)",
                      shared->names[player_idx], r, c);

      if (check_game_over(shared)) {
        int w = find_winner(shared);
        if (w != -1) {
          char msg[64];
          sprintf(msg, "WINNER:%s\n", shared->names[w]);
          for (int i = 0; i < 3; i++)
            send(shared->client_sockets[i], msg, strlen(msg), 0);
          update_winner_score(shared, w);
        } else {
          for (int i = 0; i < 3; i++)
            send(shared->client_sockets[i], "DRAW\n", 5, 0);
        }
        shared->game_over = 1;
      } else {
        shared->current_player = (shared->current_player + 1) % 3;
      }

      pthread_mutex_unlock(&shared->game_mutex);
      broadcast_board(shared);
      send_prompt(shared);
    }
    // Backward compatibility: MOVE:V r c
    else if (strncmp(buffer, "MOVE:V ", 7) == 0) {
      int r, c;
      if (sscanf(buffer + 7, "%d %d", &r, &c) != 2) {
        send(sock, "INVALID\n", 8, 0);
        continue;
      }

      pthread_mutex_lock(&shared->game_mutex);

      if (r < 0 || r > 2 || c < 0 || c > 1 || shared->v_lines[r][c]) {
        pthread_mutex_unlock(&shared->game_mutex);
        send(sock, "INVALID\n", 8, 0);
        continue;
      }

      shared->v_lines[r][c] = 1;
      check_and_score_boxes(shared, player_idx);
      add_log_message(shared, "Player %s drew vertical line at (%d,%d)",
                      shared->names[player_idx], r, c);

      if (check_game_over(shared)) {
        int w = find_winner(shared);
        if (w != -1) {
          char msg[64];
          sprintf(msg, "WINNER:%s\n", shared->names[w]);
          for (int i = 0; i < 3; i++)
            send(shared->client_sockets[i], msg, strlen(msg), 0);
          update_winner_score(shared, w);
        } else {
          for (int i = 0; i < 3; i++)
            send(shared->client_sockets[i], "DRAW\n", 5, 0);
        }
        shared->game_over = 1;
      } else {
        shared->current_player = (shared->current_player + 1) % 3;
      }

      pthread_mutex_unlock(&shared->game_mutex);
      broadcast_board(shared);
      send_prompt(shared);
    }
  }

  close(sock);
  exit(0); // Child process exits
}

/* ----------------------------- Main ----------------------------- */

int main() {
  int server_fd;
  struct sockaddr_in address;
  socklen_t addrlen = sizeof(address);

  /* 1. Setup shared memory */
  SharedGameState *shared = setup_shared_memory();
  global_shared = shared;

  /* 2. Setup signal handlers */
  signal(SIGCHLD, sigchld_handler);
  signal(SIGINT, sigint_handler);

  /* 3. Load scores */
  load_scores(shared);

  /* 4. Create threads */
  pthread_t logger_tid, scheduler_tid;
  pthread_create(&logger_tid, NULL, logger_thread, shared);
  pthread_create(&scheduler_tid, NULL, scheduler_thread, shared);

  add_log_message(shared, "Server started");

  /* 5. Create socket */
  server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    perror("socket");
    exit(1);
  }

  /* 6. Bind */
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(PORT);

  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
    perror("bind");
    exit(1);
  }

  /* 7. Listen */
  if (listen(server_fd, 3) < 0) {
    perror("listen");
    exit(1);
  }

  printf("Server listening on port %d...\n", PORT);
  add_log_message(shared, "Server listening on port %d", PORT);

  /* 8. Accept three clients and FORK for each */
  for (int i = 0; i < 3; i++) {
    printf("Waiting for Player %d...\n", i + 1);

    shared->client_sockets[i] =
        accept(server_fd, (struct sockaddr *)&address, &addrlen);

    if (shared->client_sockets[i] < 0) {
      perror("accept");
      exit(1);
    }

    /* Read player name */
    char buffer[256] = {0};
    read(shared->client_sockets[i], buffer, 256);

    if (strncmp(buffer, "NAME:", 5) == 0) {
      strcpy(shared->names[i], buffer + 5);
      printf("Player %d connected: %s\n", i + 1, shared->names[i]);
      add_log_message(shared, "Player %s connected", shared->names[i]);
    }

    shared->active[i] = 1;

    /* Assign symbol */
    shared->symbols[i] = 'A' + i;
    char symbol_msg[16];
    sprintf(symbol_msg, "SYMBOL:%c\n", shared->symbols[i]);
    send(shared->client_sockets[i], symbol_msg, strlen(symbol_msg), 0);

    /* FORK - Create child process for this client */
    pid_t pid = fork();

    if (pid < 0) {
      perror("fork");
      exit(1);
    }

    if (pid == 0) {
      /* CHILD PROCESS - Handle this client */
      close(server_fd); // Child doesn't need listening socket
      handle_client(shared, i);
      exit(0);
    }

    /* PARENT PROCESS - Continue to accept next client */
  }

  printf("All 3 players connected. Starting games...\n");
  add_log_message(shared, "All 3 players connected");

  /* 9. Game loop (in parent) */
  while (1) {
    pthread_mutex_lock(&shared->game_mutex);
    shared->game_count++;
    printf("Starting game #%d...\n", shared->game_count);
    add_log_message(shared, "=== Game #%d Started ===", shared->game_count);

    init_game(shared);
    shared->current_player = 0;
    shared->game_over = 0;
    pthread_mutex_unlock(&shared->game_mutex);

    broadcast_board(shared);
    send_prompt(shared);

    /* Wait for game to finish */
    while (1) {
      pthread_mutex_lock(&shared->game_mutex);
      if (shared->game_over) {
        pthread_mutex_unlock(&shared->game_mutex);
        break;
      }
      pthread_mutex_unlock(&shared->game_mutex);
      sleep(1);
    }

    printf("Game #%d finished. Starting next game in 5 seconds...\n",
           shared->game_count);
    sleep(5); // Pause between games
  }

  /* Cleanup (never reached in infinite loop, but good practice) */
  close(server_fd);
  munmap(shared, sizeof(SharedGameState));
  shm_unlink("/dotsboxes_game");

  return 0;
}
