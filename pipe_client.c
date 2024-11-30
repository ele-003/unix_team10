// pipe_client.c

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

#define MAX_CLIENTS 2
#define PIPE_READ 0
#define PIPE_WRITE 1

// 클라이언트와 서버 간의 파이프 파일 디스크립터
int pipe_fd[2];     // [읽기, 쓰기]
int player_id = -1; // 서버로부터 받은 플레이어 ID

pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

// 클라이언트 FIFO 이름과 서버 FIFO 이름을 저장할 변수
char client_fifo_name[32];
char server_fifo_name[32];

// 조건 변수 및 뮤텍스
pthread_cond_t turn_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t turn_mutex = PTHREAD_MUTEX_INITIALIZER;

// 스레드 종료를 위한 플래그
volatile int game_over_flag = 0;

// 스레드간 신호를 위한 플래그
volatile int your_turn = 0;

// 서버 메시지 수신 및 처리 스레드
void *listen_server(void *arg)
{
    char buffer[256];
    printf("Listener thread started for Player %d.\n", player_id);
    fflush(stdout);
    while (!game_over_flag)
    {
        int n = read(pipe_fd[PIPE_READ], buffer, sizeof(buffer));
        if (n > 0)
        {
            buffer[n] = '\0'; // 문자열 종료
            if (strncmp(buffer, "Your Turn", 9) == 0)
            {
                // 차례 시작
                pthread_mutex_lock(&file_mutex);
                // readme.txt에서 게임판 읽기
                FILE *fp = fopen("readme.txt", "r");
                if (fp == NULL)
                {
                    perror("Failed to open readme.txt");
                    pthread_mutex_unlock(&file_mutex);
                    continue;
                }
                char line[256];
                printf("\n=== Game Board ===\n");
                while (fgets(line, sizeof(line), fp))
                {
                    printf("%s", line);
                }
                fclose(fp);
                pthread_mutex_unlock(&file_mutex);

                // 조건 변수 신호를 보내어 입력 스레드가 입력을 받도록 함
                pthread_mutex_lock(&turn_mutex);
                your_turn = 1;
                pthread_cond_signal(&turn_cond);
                pthread_mutex_unlock(&turn_mutex);

                printf("Received 'Your Turn' from server.\n"); // 로그 메시지
                fflush(stdout);
            }
            else if (strncmp(buffer, "Game Over", 9) == 0)
            {
                // 게임 종료
                printf("Received 'Game Over' from server: %s\n", buffer);
                printf("Exiting client.\n");
                fflush(stdout);
                game_over_flag = 1;

                // 모든 스레드가 종료되도록 조건 변수 신호
                pthread_mutex_lock(&turn_mutex);
                pthread_cond_signal(&turn_cond);
                pthread_mutex_unlock(&turn_mutex);

                // 파이프 닫기
                close(pipe_fd[PIPE_READ]);
                close(pipe_fd[PIPE_WRITE]);

                break; // 스레드 종료
            }
            else if (strncmp(buffer, "Invalid Move", 12) == 0)
            {
                // 잘못된 수
                printf("Invalid move. Try again.\n");
                fflush(stdout);
            }
            else
            {
                // 기타 메시지 처리
                printf("Received unknown message from server: %s\n", buffer);
                fflush(stdout);
            }
        }
        else if (n == 0)
        {
            // 파이프가 닫혔을 때
            printf("Server closed the connection.\n");
            fflush(stdout);
            game_over_flag = 1;

            // 모든 스레드가 종료되도록 조건 변수 신호
            pthread_mutex_lock(&turn_mutex);
            pthread_cond_signal(&turn_cond);
            pthread_mutex_unlock(&turn_mutex);

            break; // 스레드 종료
        }
        else
        {
            perror("read failed");
            break; // 스레드 종료
        }
    }
    printf("Listener thread exiting for Player %d.\n", player_id);
    fflush(stdout);
    pthread_exit(NULL);
}

void *input_handler(void *arg)
{
    printf("Input handler thread started for Player %d.\n", player_id);
    fflush(stdout);
    while (!game_over_flag)
    {
        // 조건 변수를 기다림
        pthread_mutex_lock(&turn_mutex);
        while (!your_turn && !game_over_flag)
        {
            pthread_cond_wait(&turn_cond, &turn_mutex);
        }
        if (game_over_flag)
        {
            pthread_mutex_unlock(&turn_mutex);
            break;
        }
        your_turn = 0; // 플래그 초기화
        pthread_mutex_unlock(&turn_mutex);

        // 시간 측정 시작
        struct timespec start_time, end_time;
        clock_gettime(CLOCK_MONOTONIC, &start_time);

        // 수 입력
        int row, col;
        printf("Enter your move (row and column): ");
        fflush(stdout); // 출력 버퍼 플러시

        // 표준 입력을 감시하기 위해 select 함수 사용
        while (!game_over_flag)
        {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(STDIN_FILENO, &readfds);

            struct timeval tv;
            tv.tv_sec = 1; // 1초 타임아웃
            tv.tv_usec = 0;

            int retval = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv);
            if (retval == -1)
            {
                perror("select()");
                break;
            }
            else if (retval)
            {
                // 입력이 있음
                char input_buffer[256];
                if (fgets(input_buffer, sizeof(input_buffer), stdin) != NULL)
                {
                    // 입력 처리
                    if (sscanf(input_buffer, "%d %d", &row, &col) != 2)
                    {
                        printf("Invalid input format.\n");
                        fflush(stdout);
                        printf("Enter your move (row and column): ");
                        fflush(stdout);
                        continue;
                    }
                    break; // 유효한 입력을 받았으므로 루프 탈출
                }
                else
                {
                    // EOF 또는 오류
                    break;
                }
            }
            else
            {
                // 타임아웃 발생, game_over_flag 확인
                if (game_over_flag)
                {
                    break;
                }
            }
        }

        if (game_over_flag)
        {
            break;
        }

        // 시간 측정 종료
        clock_gettime(CLOCK_MONOTONIC, &end_time);
        double elapsed = (end_time.tv_sec - start_time.tv_sec) +
                         (end_time.tv_nsec - start_time.tv_nsec) / 1e9;
        printf("Input Time: %.3f seconds\n", elapsed);
        fflush(stdout);

        // 서버로 수 전송 (row, col, elapsed_time)
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "%d %d %.3f", row, col, elapsed);
        if (write(pipe_fd[PIPE_WRITE], buffer, strlen(buffer) + 1) == -1)
        {
            perror("write to server failed");
            continue;
        }
    }
    printf("Input handler thread exiting for Player %d.\n", player_id);
    fflush(stdout);
    pthread_exit(NULL);
}

// 상태 모니터링 스레드 (추가적인 기능을 수행할 수 있음)
void *status_monitor(void *arg)
{
    printf("Status monitor thread started for Player %d.\n", player_id);
    fflush(stdout);
    while (!game_over_flag)
    {
        // 클라이언트의 상태를 주기적으로 확인하거나 추가적인 작업을 수행
        sleep(1); // 예시로 1초마다 확인
    }
    printf("Status monitor thread exiting for Player %d.\n", player_id);
    fflush(stdout);
    pthread_exit(NULL);
}

int main(int argc, char *argv[])
{
    // 인자 검사: 플레이어 ID (0 또는 1) 필요
    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s <player_id (0 or 1)>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    player_id = atoi(argv[1]);
    if (player_id < 0 || player_id >= MAX_CLIENTS)
    {
        fprintf(stderr, "Invalid player_id. Must be 0 or 1.\n");
        exit(EXIT_FAILURE);
    }

    // 플레이어 ID에 따른 FIFO 이름 설정
    snprintf(client_fifo_name, sizeof(client_fifo_name), "client%d_fifo", player_id);
    snprintf(server_fifo_name, sizeof(server_fifo_name), "server%d_fifo", player_id);

    // 서버로 접속 알림을 위해 FIFO 열기
    pipe_fd[PIPE_WRITE] = open(client_fifo_name, O_WRONLY);
    if (pipe_fd[PIPE_WRITE] == -1)
    {
        perror("Failed to open client FIFO for writing");
        exit(EXIT_FAILURE);
    }

    // 서버로부터 메시지 수신을 위한 FIFO 열기
    pipe_fd[PIPE_READ] = open(server_fifo_name, O_RDONLY);
    if (pipe_fd[PIPE_READ] == -1)
    {
        perror("Failed to open server FIFO for reading");
        close(pipe_fd[PIPE_WRITE]);
        exit(EXIT_FAILURE);
    }

    printf("Connected to server as Player %d.\n", player_id);
    fflush(stdout);

    // 서버로부터 메시지 수신 스레드 시작
    pthread_t listener_thread;
    if (pthread_create(&listener_thread, NULL, listen_server, NULL) != 0)
    {
        perror("Failed to create listener thread");
        exit(EXIT_FAILURE);
    }

    // 사용자 입력을 처리하는 스레드 시작
    pthread_t input_thread;
    if (pthread_create(&input_thread, NULL, input_handler, NULL) != 0)
    {
        perror("Failed to create input thread");
        exit(EXIT_FAILURE);
    }

    // 상태 모니터링 스레드 생성
    pthread_t monitor_thread;
    if (pthread_create(&monitor_thread, NULL, status_monitor, NULL) != 0)
    {
        perror("Failed to create status monitor thread");
        exit(EXIT_FAILURE);
    }

    // 메인 스레드는 리스너, 입력, 모니터 스레드가 종료될 때까지 대기
    pthread_join(listener_thread, NULL);
    pthread_join(input_thread, NULL);
    pthread_join(monitor_thread, NULL);

    // 리소스 정리
    pthread_mutex_destroy(&file_mutex);
    pthread_cond_destroy(&turn_cond);
    pthread_mutex_destroy(&turn_mutex);

    printf("Client exiting.\n");
    fflush(stdout);

    return 0;
}