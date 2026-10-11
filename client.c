#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define PORT 8080

typedef struct {
  char my_name[50];
  char my_symbol;

  // Dots & Boxes game state
  int h_lines[2][3]; // Horizontal lines: 2 rows × 3 cols
  int v_lines[3][2]; // Vertical lines: 3 rows × 2 cols
  char boxes[2][2];  // Box ownership: 2×2 grid
  int scores[3];     // Scores for all 3 players
} ClientState;

void display_board(ClientState *state) {
  printf("\n");
  printf("┌─────────────────────────────────────────┐\n");
  printf("│           DOTS & BOXES                  │\n");
  printf("├─────────────────────────────────────────┤\n");
  printf("│ Scores:  A:%d   B:%d   C:%d            │\n", 
         state->scores[0], state->scores[1], state->scores[2]);
  printf("└─────────────────────────────────────────┘\n\n");

  // Line numbering:
  // Horizontal: 1-6 (h_lines[0-1][0-2])
  // Vertical: 7-12 (v_lines[0-2][0-1])
  
  // Row 0 (top row) - horizontal line numbers
  printf("      ");
  for (int c = 0; c < 3; c++) {
    int line_num = c + 1;  // Lines 1-3
    if (state->h_lines[0][c])
      printf("     ");
    else
      printf("  %d  ", line_num);
  }
  printf("\n");
  
  // Top dots row
  printf("   •───•───•───•\n");
  
  // Middle rows with vertical lines and boxes
  for (int r = 0; r < 2; r++) {
    // Vertical line numbers and boxes
    int v_num1 = 7 + r * 2;    // Left vertical: 7, 9, 11
    int v_num2 = v_num1 + 1;   // Right vertical: 8, 10, 12
    
    // Row with box content
    printf(" %d ", state->v_lines[r][0] ? ' ' : v_num1);
    printf("│ %c ", state->boxes[r][0] == ' ' ? ' ' : state->boxes[r][0]);
    printf("│ %c ", state->boxes[r][1] == ' ' ? ' ' : state->boxes[r][1]);
    printf("│ %d ", state->v_lines[r][1] ? ' ' : v_num2);
    printf("\n");
    
    // Horizontal line between boxes (shows line numbers for gap)
    printf("   ");
    for (int c = 0; c < 3; c++) {
      int h_num = 4 + r * 3 + c;  // Lines 4-6 for row 1
      if (state->h_lines[r][c])
        printf("│   ");
      else
        printf("│ %d ", h_num);
    }
    printf("│\n");
    
    // Horizontal connector line
    printf("   ");
    for (int c = 0; c < 3; c++) {
      if (state->h_lines[r + 1][c])
        printf("│   ");
      else
        printf("├───");
    }
    printf("│\n");
  }
  
  // Bottom row (last boxes)
  printf(" %d ", state->v_lines[2][0] ? ' ' : 11);
  printf("│ %c ", state->boxes[1][0] == ' ' ? ' ' : state->boxes[1][0]);
  printf("│ %c ", state->boxes[1][1] == ' ' ? ' ' : state->boxes[1][1]);
  printf("│ %d ", state->v_lines[2][1] ? ' ' : 12);
  printf("\n");
  
  // Bottom dots row
  printf("   •───•───•───•\n");
  
  // Bottom horizontal line numbers
  printf("      ");
  for (int c = 0; c < 3; c++) {
    int line_num = c + 4;  // Lines 4-6
    if (state->h_lines[1][c])
      printf("     ");
    else
      printf("  %d  ", line_num);
  }
  printf("\n\n");
  
  // Legend
  printf("┌─────────────────────────────────────────────┐\n");
  printf("│ LINE NUMBER REFERENCE:                     │\n");
  printf("├─────────────────────────────────────────────┤\n");
  printf("│   TOP horizontal:  1   2   3                │\n");
  printf("│   LEFT vertical:  7 8   9 10  11 12 RIGHT    │\n");
  printf("│   BOTTOM horizontal: 4   5   6               │\n");
  printf("└─────────────────────────────────────────────┘\n\n");
}

int main() {
  int sock;
  struct sockaddr_in serv_addr;
  ClientState state;

  /* Create socket */
  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    perror("socket");
    exit(1);
  }

  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(PORT);
  inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

  if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
    perror("connect");
    exit(1);
  }

  printf("Connected to server!\n");

  /* Send name */
  printf("Enter your name: ");
  fflush(stdout);
  if (fgets(state.my_name, 50, stdin) == NULL) {
    printf("Failed to read name.\n");
    exit(1);
  }
  state.my_name[strcspn(state.my_name, "\n")] = '\0';

  char msg[80];
  sprintf(msg, "NAME:%s\n", state.my_name);
  send(sock, msg, strlen(msg), 0);

  /* Initialize local game state */
  for (int r = 0; r < 2; r++)
    for (int c = 0; c < 3; c++)
      state.h_lines[r][c] = 0;

  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 2; c++)
      state.v_lines[r][c] = 0;

  for (int r = 0; r < 2; r++)
    for (int c = 0; c < 2; c++)
      state.boxes[r][c] = ' ';

  for (int i = 0; i < 3; i++)
    state.scores[i] = 0;

  /* Game Loop */
  char buffer[1024] = {0};
  int len = 0;
  int waiting_for_input = 0;

  while (1) {
    // Check if we need to read from socket
    fd_set readfds;
    struct timeval tv;
    
    FD_ZERO(&readfds);
    FD_SET(sock, &readfds);
    FD_SET(STDIN_FILENO, &readfds);
    
    int maxfd = sock > STDIN_FILENO ? sock : STDIN_FILENO;
    
    tv.tv_sec = 0;
    tv.tv_usec = 100000;  // 100ms timeout
    
    int activity = select(maxfd + 1, &readfds, NULL, NULL, &tv);
    
    if (activity < 0) {
      perror("select");
      break;
    }
    
    // Handle socket data
    if (FD_ISSET(sock, &readfds)) {
      int val = read(sock, buffer + len, 1024 - len - 1);
      if (val <= 0) {
        printf("\nServer disconnected.\n");
        break;
      }
      
      len += val;
      buffer[len] = '\0';
      
      char *newline;
      while ((newline = strchr(buffer, '\n')) != NULL) {
        *newline = '\0';
        char *msg_ptr = buffer;
        
        // Process message
        if (strncmp(msg_ptr, "SYMBOL:", 7) == 0) {
          state.my_symbol = msg_ptr[7];
          printf("\nYour symbol: %c\n", state.my_symbol);
        } else if (strncmp(msg_ptr, "BOARD:", 6) == 0) {
          char *data = msg_ptr + 6;
          
          // Parse horizontal lines (6 bytes)
          for (int r = 0, idx = 0; r < 2; r++)
            for (int c = 0; c < 3; c++)
              state.h_lines[r][c] = (data[idx++] == '1');
          
          // Parse vertical lines (6 bytes)
          for (int r = 0, idx = 6; r < 3; r++)
            for (int c = 0; c < 2; c++)
              state.v_lines[r][c] = (data[idx++] == '1');
          
          // Parse boxes (4 bytes)
          for (int r = 0, idx = 12; r < 2; r++)
            for (int c = 0; c < 2; c++)
              state.boxes[r][c] = data[idx++];
          
          // Parse scores (after ':')
          char *scores_ptr = strchr(data, ':');
          if (scores_ptr) {
            sscanf(scores_ptr + 1, "%d,%d,%d", &state.scores[0], &state.scores[1],
                   &state.scores[2]);
          }
          
          display_board(&state);
          waiting_for_input = 0;
        } else if (strcmp(msg_ptr, "PROMPT") == 0) {
          printf("\n>>> YOUR TURN! <<<\n");
          printf("Enter line number (1-12): ");
          fflush(stdout);
          waiting_for_input = 1;
        } else if (strcmp(msg_ptr, "WAIT") == 0) {
          if (!waiting_for_input) {
            printf("\nWaiting for opponent...\n");
            fflush(stdout);
          }
        } else if (strcmp(msg_ptr, "INVALID") == 0) {
          printf("\n>>> INVALID MOVE! <<<\n");
          printf("Enter line number (1-12): ");
          fflush(stdout);
          waiting_for_input = 1;
        } else if (strncmp(msg_ptr, "WINNER:", 7) == 0) {
          printf("\n╔═══════════════════════════════════╗\n");
          printf("║         GAME OVER!                 ║\n");
          printf("╠═══════════════════════════════════╣\n");
          printf("║ Winner: %-20s      ║\n", msg_ptr + 7);
          if (strcmp(msg_ptr + 7, state.my_name) == 0)
            printf("║       🎉🎉🎉 YOU WIN! 🎉🎉🎉          ║\n");
          else
            printf("║       Better luck next time!       ║\n");
          printf("╚═══════════════════════════════════╝\n");
          printf("Starting next game in 5 seconds...\n");
          fflush(stdout);
          waiting_for_input = 0;
          sleep(5);
          // Continue loop to receive new BOARD
        } else if (strcmp(msg_ptr, "DRAW") == 0) {
          printf("\n╔═══════════════════════════════════╗\n");
          printf("║         GAME OVER!                ║\n");
          printf("║         It's a DRAW!              ║\n");
          printf("╚═══════════════════════════════════╝\n");
          printf("Starting next game in 5 seconds...\n");
          fflush(stdout);
          waiting_for_input = 0;
          sleep(5);
        } else if (strcmp(msg_ptr, "DISCONNECT") == 0) {
          printf("\nOpponent disconnected.\n");
          goto end_game;
        }
        
        // Move remaining data to start
        int msg_len = (newline - buffer) + 1;
        int remaining = len - msg_len;
        memmove(buffer, newline + 1, remaining);
        len = remaining;
        buffer[len] = '\0';
      }
    }
    
    // Handle stdin input
    if (waiting_for_input && FD_ISSET(STDIN_FILENO, &readfds)) {
      char input[64];
      if (fgets(input, sizeof(input), stdin) != NULL) {
        int line_num = atoi(input);
        if (line_num >= 1 && line_num <= 12) {
          char sendbuf[64];
          sprintf(sendbuf, "MOVE:%d\n", line_num);
          send(sock, sendbuf, strlen(sendbuf), 0);
          waiting_for_input = 0;
        } else {
          printf("Invalid number! Enter 1-12: ");
          fflush(stdout);
        }
      }
    }
  }

end_game:

  close(sock);
  return 0;
}

