#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>
#include <string.h>

#define SHM_KEY 60104      // 공유 메모리 키를 60103으로 설정
#define BOARD_SIZE 9
#define MAX_CLIENTS 2

typedef struct {
    char board[BOARD_SIZE];
    int turn;       // 현재 차례: 0 또는 1
    int game_over;  // 게임 종료 여부
    int winner;     // 승자: -1(무승부), 0 또는 1
    int client_count;
    int ready[MAX_CLIENTS];
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} SharedMemory;

SharedMemory* shared_mem;

void initialize_board() {
    for (int i = 0; i < BOARD_SIZE; i++) {
        shared_mem->board[i] = ' ';
    }
}

void print_board() {
    printf("\n");
    for (int i = 0; i < BOARD_SIZE; i++) {
        printf(" %c ", shared_mem->board[i]);
        if ((i + 1) % 3 == 0) {
            printf("\n");
            if (i < 6) printf("---+---+---\n");
        }
        else {
            printf("|");
        }
    }
    printf("\n");
}

int check_winner() {
    int win_patterns[8][3] = {
        {0,1,2}, {3,4,5}, {6,7,8}, // Rows
        {0,3,6}, {1,4,7}, {2,5,8}, // Columns
        {0,4,8}, {2,4,6}           // Diagonals
    };

    for (int i = 0; i < 8; i++) {
        if (shared_mem->board[win_patterns[i][0]] != ' ' &&
            shared_mem->board[win_patterns[i][0]] == shared_mem->board[win_patterns[i][1]] &&
            shared_mem->board[win_patterns[i][1]] == shared_mem->board[win_patterns[i][2]]) {
            return (shared_mem->board[win_patterns[i][0]] == 'X') ? 0 : 1;
        }
    }
    return -1;
}

void* game_manager_thread(void* arg) {
    while (!shared_mem->game_over) {
        pthread_mutex_lock(&shared_mem->mutex);

        // 모든 클라이언트가 준비될 때까지 대기
        while (shared_mem->client_count < MAX_CLIENTS) {
            pthread_cond_wait(&shared_mem->cond, &shared_mem->mutex);
        }

        // 승리 조건 확인
        shared_mem->winner = check_winner();
        if (shared_mem->winner != -1) {
            shared_mem->game_over = 1;
        }
        else {
            // 무승부 확인
            int empty_spaces = 0;
            for (int i = 0; i < BOARD_SIZE; i++) {
                if (shared_mem->board[i] == ' ') {
                    empty_spaces++;
                }
            }
            if (empty_spaces == 0) {
                shared_mem->game_over = 1;
                shared_mem->winner = -1;
            }
        }

        if (shared_mem->game_over) {
            // 게임이 종료되었으므로 모든 스레드를 깨웁니다.
            pthread_cond_broadcast(&shared_mem->cond);
        }

        pthread_mutex_unlock(&shared_mem->mutex);
        usleep(100000); // 0.1초 대기
    }
    return NULL;
}

void* display_thread(void* arg) {
    while (!shared_mem->game_over) {
        pthread_mutex_lock(&shared_mem->mutex);
        system("clear");
        printf("틱택토 게임 서버\n");
        print_board();
        pthread_mutex_unlock(&shared_mem->mutex);
        sleep(1);
    }

    // 게임 종료 시 최종 보드 상태 출력
    pthread_mutex_lock(&shared_mem->mutex);
    system("clear");
    printf("게임 종료!\n");
    print_board();
    if (shared_mem->winner == -1) {
        printf("무승부입니다.\n");
    }
    else {
        printf("플레이어 %d 승리!\n", shared_mem->winner);
    }
    pthread_mutex_unlock(&shared_mem->mutex);

    return NULL;
}

void* client_handler_thread(void* arg) {
    int player_id = *(int*)arg;
    free(arg);

    while (!shared_mem->game_over) {
        pthread_mutex_lock(&shared_mem->mutex);

        // 자신의 차례가 아닐 때 대기
        while (shared_mem->turn != player_id && !shared_mem->game_over) {
            pthread_cond_wait(&shared_mem->cond, &shared_mem->mutex);
        }

        if (shared_mem->game_over) {
            pthread_mutex_unlock(&shared_mem->mutex);
            break;
        }

        // 클라이언트의 입력 대기 (실제 입력은 클라이언트에서 처리)
        pthread_mutex_unlock(&shared_mem->mutex);
        usleep(100000); // 0.1초 대기
    }
    return NULL;
}

int main() {
    int shm_id = shmget(SHM_KEY, sizeof(SharedMemory), IPC_CREAT | 0666);

    if (shm_id < 0) {
        perror("shmget failed");
        exit(1);
    }

    shared_mem = (SharedMemory*)shmat(shm_id, NULL, 0);

    if (shared_mem == (void*)-1) {
        perror("shmat failed");
        exit(1);
    }

    // 공유 메모리 초기화
    initialize_board();
    shared_mem->turn = 0;
    shared_mem->game_over = 0;
    shared_mem->winner = -1;
    shared_mem->client_count = 0;
    memset(shared_mem->ready, 0, sizeof(shared_mem->ready));

    // 뮤텍스와 조건 변수 초기화
    pthread_mutexattr_t mutex_attr;
    pthread_condattr_t cond_attr;

    pthread_mutexattr_init(&mutex_attr);
    pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);

    pthread_condattr_init(&cond_attr);
    pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED);

    pthread_mutex_init(&shared_mem->mutex, &mutex_attr);
    pthread_cond_init(&shared_mem->cond, &cond_attr);

    printf("틱택토 서버가 시작되었습니다...\n");

    // 스레드 생성
    pthread_t game_thread, display_t;
    pthread_create(&game_thread, NULL, game_manager_thread, NULL);
    pthread_create(&display_t, NULL, display_thread, NULL);

    // 클라이언트 핸들러 스레드 생성
    pthread_t client_threads[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; i++) {
        int* arg = malloc(sizeof(*arg));
        *arg = i;
        pthread_create(&client_threads[i], NULL, client_handler_thread, arg);
    }

    // 스레드 종료 대기
    pthread_join(game_thread, NULL);
    pthread_join(display_t, NULL);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        pthread_join(client_threads[i], NULL);
    }

    // 뮤텍스와 조건 변수 파괴
    pthread_mutex_destroy(&shared_mem->mutex);
    pthread_cond_destroy(&shared_mem->cond);

    // 공유 메모리 분리 및 삭제
    shmdt(shared_mem);
    shmctl(shm_id, IPC_RMID, NULL);

    printf("서버를 종료합니다.\n");

    return 0;
}

