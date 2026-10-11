# Concurrent Multiplayer Dots-and-Boxes Game

A 3-player networked **Dots-and-Boxes** terminal game implemented in **C**, demonstrating operating systems concepts including POSIX Inter-Process Communication (IPC), shared memory, process synchronization, and socket programming.

---

## 🌟 Operating System Concepts Demonstrated

1. **Network Sockets (TCP/IP):** Multi-client network architecture communicating over port 8080.
2. **POSIX Shared Memory (`mmap` / `sys/mman.h`):** Process-shared game state (`SharedGameState`) mapped across processes.
3. **Synchronization & Concurrency:**
   - **Process-shared Mutexes (`pthread_mutex_t`):** Mutex protection for game state, score updates, and thread-safe logging.
   - **POSIX Semaphores (`sem_t`):** Coordinated producer-consumer logging queue.
4. **Process Management:** Handled signals (`SIGINT`, `SIGCHLD`) and process lifecycle management (`fork`, `wait`).
5. **Colorized Terminal UI:** ANSI escape code rendering for game board, turn indicators, and real-time scoreboard.

---

## 🎮 How to Play

### 1. Compile
```bash
make
```

### 2. Start Game Server
```bash
./server
```

### 3. Connect 3 Players
Open 3 separate terminal tabs/windows and in each run:
```bash
./client
```

Enter player names and take turns completing boxes on the grid!
