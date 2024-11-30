// pipe_server.c

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <time.h>
#include <semaphore.h>

#define MAX_CLIENTS 2
#define BOARD_SIZE 3
#define SEM_NAME_FORMAT "/sem_player_%d"
#define PIPE_READ 0
#define PIPE_WRITE 1

typedef struct
{
    char board[BOARD_SIZE][BOARD_SIZE];
    int turn;   // 현재 턴인 플레이어 ID (0 또는 1)
    int winner; // -1: 게임 진행 중, 0 또는 1: 승자, 2: 무승부
} GameState;

typedef struct
{
    int id;
    int pipe_fd[2]; // [읽기, 쓰기]
    char sem_name[64];
    sem_t *turn_sem;
} ClientInfo;

// 전역 변수 정의
GameState game;
pthread_mutex_t game_mutex = PTHREAD_MUTEX_INITIALIZER; // 게임 상태 보호를 위한 뮤텍스
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER; // 파일 접근 보호를 위한 뮤텍스
ClientInfo clients[MAX_CLIENTS];
int client_count = 0;
char *fifo_names[MAX_CLIENTS] = {"client0_fifo", "client1_fifo"};
char *server_fifos[MAX_CLIENTS] = {"server0_fifo", "server1_fifo"};
struct timespec game_start_time, game_end_time;
double input_times[MAX_CLIENTS] = {0.0};
volatile int game_over_flag = 0; // 게임 종료 플래그

// 게임 초기화 함수
void init_game(GameState *game)
{
    for (int i = 0; i < BOARD_SIZE; i++)
        for (int j = 0; j < BOARD_SIZE; j++)
            game->board[i][j] = ' ';
    game->turn = 0;
    game->winner = -1;
}

// 승리 조건 체크 함수
int check_winner(GameState *game)
{
    // 가로, 세로, 대각선 체크
    for (int i = 0; i < BOARD_SIZE; i++)
    {
        // 가로
        if (game->board[i][0] != ' ' &&
            game->board[i][0] == game->board[i][1] &&
            game->board[i][1] == game->board[i][2])
        {
            printf("Player %d wins by row %d!\n", game->board[i][0] == 'X' ? 0 : 1, i);
            fflush(stdout);
            return game->board[i][0] == 'X' ? 0 : 1;
        }

        // 세로
        if (game->board[0][i] != ' ' &&
            game->board[0][i] == game->board[1][i] &&
            game->board[1][i] == game->board[2][i])
        {
            printf("Player %d wins by column %d!\n", game->board[0][i] == 'X' ? 0 : 1, i);
            fflush(stdout);
            return game->board[0][i] == 'X' ? 0 : 1;
        }
    }

    // 대각선
    if (game->board[0][0] != ' ' &&
        game->board[0][0] == game->board[1][1] &&
        game->board[1][1] == game->board[2][2])
    {
        printf("Player %d wins by main diagonal!\n", game->board[0][0] == 'X' ? 0 : 1);
        fflush(stdout);
        return game->board[0][0] == 'X' ? 0 : 1;
    }

    if (game->board[0][2] != ' ' &&
        game->board[0][2] == game->board[1][1] &&
        game->board[1][1] == game->board[2][0])
    {
        printf("Player %d wins by anti-diagonal!\n", game->board[0][2] == 'X' ? 0 : 1);
        fflush(stdout);
        return game->board[0][2] == 'X' ? 0 : 1;
    }

    return -1; // 아직 승자가 없음
}

// 무승부 체크 함수
int is_draw(GameState *game)
{
    for (int i = 0; i < BOARD_SIZE; i++)
        for (int j = 0; j < BOARD_SIZE; j++)
            if (game->board[i][j] == ' ')
                return 0; // 아직 빈 공간 있음
    printf("The game is a draw.\n");
    fflush(stdout);
    return 1; // 무승부
}

// 수를 두는 함수
int make_move(GameState *game, int player_id, int row, int col)
{
    if (row < 0 || row >= BOARD_SIZE || col < 0 || col >= BOARD_SIZE)
        return -1; // 잘못된 좌표
    if (game->board[row][col] != ' ')
        return -1; // 이미 수가 존재함
    game->board[row][col] = (player_id == 0) ? 'X' : 'O';
    return 0; // 성공
}

// 게임 모니터링 스레드
void *game_monitor(void *arg)
{
    printf("Game monitor thread started.\n");
    fflush(stdout);

    while (!game_over_flag)
    {
        // 게임 상태 확인
        pthread_mutex_lock(&game_mutex);
        game.winner = check_winner(&game);
        if (game.winner != -1 || is_draw(&game))
        {
            game_over_flag = 1;
            pthread_mutex_unlock(&game_mutex);
            break;
        }
        pthread_mutex_unlock(&game_mutex);

        // 잠시 대기
        sleep(1);
    }

    printf("Game monitor thread detected game over.\n");
    fflush(stdout);

    // 게임 종료 메시지 전송 및 세마포어 해제
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "Game Over|Winner:%d", game.winner);
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (write(clients[i].pipe_fd[PIPE_WRITE], buffer, strlen(buffer) + 1) == -1)
        {
            perror("write to client failed");
        }
        // 파이프 닫기
        close(clients[i].pipe_fd[PIPE_WRITE]);

        // 세마포어 해제
        if (sem_post(clients[i].turn_sem) == -1)
        {
            perror("sem_post failed");
        }
    }

    pthread_exit(NULL);
}

// 클라이언트 핸들러 함수
void *client_handler(void *arg)
{
    ClientInfo *client = (ClientInfo *)arg;
    printf("Client handler for Player %d started.\n", client->id);
    fflush(stdout);
    char buffer[256];

    while (!game_over_flag)
    {
        // 차례 대기
        if (sem_wait(client->turn_sem) == -1)
        {
            perror("sem_wait failed");
            break;
        }

        if (game_over_flag)
        {
            break;
        }

        // 현재 게임판 및 차례 알림
        pthread_mutex_lock(&file_mutex);
        FILE *fp = fopen("readme.txt", "w");
        if (fp == NULL)
        {
            perror("Failed to open readme.txt");
            pthread_mutex_unlock(&file_mutex);
            continue;
        }
        fprintf(fp, "Turn:%d\n", game.turn); // 정확한 턴 정보 기록
        for (int i = 0; i < BOARD_SIZE; i++)
        {
            fprintf(fp, "%c|%c|%c\n", game.board[i][0], game.board[i][1], game.board[i][2]);
        }
        fclose(fp);
        pthread_mutex_unlock(&file_mutex);

        // 차례 시작 알림
        snprintf(buffer, sizeof(buffer), "Your Turn");
        if (write(client->pipe_fd[PIPE_WRITE], buffer, strlen(buffer) + 1) == -1)
        {
            perror("write Your Turn to client failed");
            continue;
        }

        // 현재 플레이어의 턴임을 서버 콘솔에 출력
        printf("Player %d's turn.\n", client->id);
        fflush(stdout); // 즉시 출력

        // 클라이언트의 수 입력 대기
        int n = read(client->pipe_fd[PIPE_READ], buffer, sizeof(buffer));
        if (n > 0)
        {
            int row, col;
            double elapsed_time;
            // 클라이언트가 "row col elapsed_time" 형식으로 전송
            if (sscanf(buffer, "%d %d %lf", &row, &col, &elapsed_time) != 3)
            {
                fprintf(stderr, "Invalid input format from client %d.\n", client->id);
                fflush(stderr);
                snprintf(buffer, sizeof(buffer), "Invalid Move");
                if (write(client->pipe_fd[PIPE_WRITE], buffer, strlen(buffer) + 1) == -1)
                {
                    perror("write Invalid Move to client failed");
                }
                // 현재 클라이언트의 세마포어 다시 해제
                if (sem_post(client->turn_sem) == -1)
                {
                    perror("sem_post failed");
                }
                continue;
            }

            // 입력 시간 누적
            input_times[client->id] += elapsed_time;

            pthread_mutex_lock(&game_mutex);
            if (make_move(&game, client->id, row, col) == 0)
            {
                // 다음 차례로 전환
                game.turn = 1 - game.turn;
                pthread_mutex_unlock(&game_mutex);

                // 다음 플레이어의 세마포어 해제
                if (sem_post(clients[game.turn].turn_sem) == -1)
                {
                    perror("sem_post failed");
                }
            }
            else
            {
                // 잘못된 수, 다시 시도 요청
                pthread_mutex_unlock(&game_mutex);
                snprintf(buffer, sizeof(buffer), "Invalid Move");
                if (write(client->pipe_fd[PIPE_WRITE], buffer, strlen(buffer) + 1) == -1)
                {
                    perror("write Invalid Move to client failed");
                }
                // 현재 클라이언트의 세마포어 다시 해제
                if (sem_post(client->turn_sem) == -1)
                {
                    perror("sem_post failed");
                }
            }
        }
        else if (n == 0)
        {
            // 파이프가 닫혔을 때
            printf("Client %d disconnected.\n", client->id);
            fflush(stdout);
            break; // 스레드 종료
        }
        else
        {
            perror("read failed");
            break; // 스레드 종료
        }
    }
    printf("Client handler for Player %d exiting.\n", client->id);
    fflush(stdout);
    pthread_exit(NULL);
}

int main()
{
    // 게임 초기화
    init_game(&game);

    // 기존 FIFO 파일 제거 후 생성
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        unlink(fifo_names[i]); // 기존 FIFO 파일 제거
        unlink(server_fifos[i]);
        if (mkfifo(fifo_names[i], 0666) == -1)
        {
            perror("Failed to create client FIFO");
            exit(EXIT_FAILURE);
        }
        if (mkfifo(server_fifos[i], 0666) == -1)
        {
            perror("Failed to create server FIFO");
            exit(EXIT_FAILURE);
        }
    }

    printf("Waiting for clients to connect...\n");
    fflush(stdout);

    // 클라이언트 접속 대기
    while (client_count < MAX_CLIENTS)
    {
        // 클라이언트 ID에 따라 FIFO 열기
        int id = client_count;
        int fd_read = open(fifo_names[id], O_RDONLY);
        if (fd_read == -1)
        {
            perror("Failed to open client FIFO for reading");
            continue;
        }
        int fd_write = open(server_fifos[id], O_WRONLY);
        if (fd_write == -1)
        {
            perror("Failed to open server FIFO for writing");
            close(fd_read);
            continue;
        }

        // 세마포어 이름 설정
        snprintf(clients[id].sem_name, sizeof(clients[id].sem_name), SEM_NAME_FORMAT, id);
        // 세마포어 초기화 (이름 있는 세마포어)
        clients[id].turn_sem = sem_open(clients[id].sem_name, O_CREAT, 0644, 0);
        if (clients[id].turn_sem == SEM_FAILED)
        {
            perror("Failed to open semaphore");
            close(fd_read);
            close(fd_write);
            continue;
        }

        // 클라이언트 정보 설정
        clients[id].id = id;
        clients[id].pipe_fd[PIPE_READ] = fd_read;
        clients[id].pipe_fd[PIPE_WRITE] = fd_write;

        client_count++;
        printf("Client %d connected.\n", id);
        fflush(stdout);
    }

    // 게임 시작 시간 기록 (두 클라이언트가 연결된 시점)
    clock_gettime(CLOCK_MONOTONIC, &game_start_time);

    // 게임 모니터링 스레드 생성
    pthread_t monitor_thread;
    if (pthread_create(&monitor_thread, NULL, game_monitor, NULL) != 0)
    {
        perror("Failed to create game monitor thread");
        exit(EXIT_FAILURE);
    }

    // 첫 번째 플레이어의 세마포어 해제 (선공)
    if (sem_post(clients[0].turn_sem) == -1)
    {
        perror("sem_post failed");
    }

    // 클라이언트별로 스레드 생성 (클라이언트 핸들러 스레드)
    pthread_t client_threads[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (pthread_create(&client_threads[i], NULL, client_handler, &clients[i]) != 0)
        {
            perror("Failed to create client handler thread");
            exit(EXIT_FAILURE);
        }
    }

    // 게임 모니터링 스레드가 게임 종료를 감지할 때까지 대기
    pthread_join(monitor_thread, NULL);

    // 모든 클라이언트 핸들러 스레드가 종료될 때까지 대기
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        pthread_join(client_threads[i], NULL);
    }

    // 게임 종료 시간 기록
    clock_gettime(CLOCK_MONOTONIC, &game_end_time);

    // 전체 실행 시간 계산
    double total_runtime = (game_end_time.tv_sec - game_start_time.tv_sec) +
                           (game_end_time.tv_nsec - game_start_time.tv_nsec) / 1e9;

    // 입력 시간 합계 계산
    double total_input_time = 0.0;
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        total_input_time += input_times[i];
    }

    // 결과 출력
    printf("Game ended.\n");
    if (game.winner == 2 || game.winner == -1)
    {
        printf("Result: Draw.\n");
    }
    else
    {
        printf("Result: Player %d wins!\n", game.winner);
    }

    printf("Total Runtime: %.3f seconds\n", total_runtime);
    printf("Total Input Time: %.3f seconds\n", total_input_time);
    printf("Adjusted Time (Runtime - Input Time): %.3f seconds\n", total_runtime - total_input_time);

    // 리소스 정리
    pthread_mutex_destroy(&game_mutex);
    pthread_mutex_destroy(&file_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        sem_close(clients[i].turn_sem);
        sem_unlink(clients[i].sem_name);
        close(clients[i].pipe_fd[PIPE_READ]);
        // Write end은 이미 닫혔으므로 재닫기 시도 필요 없음
        // close(clients[i].pipe_fd[PIPE_WRITE]); // 이미 닫힘
        unlink(fifo_names[i]);
        unlink(server_fifos[i]);
    }

    printf("Server exiting.\n");
    fflush(stdout);

    return 0;
}