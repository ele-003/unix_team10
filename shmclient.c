#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>

#define SHM_KEY 60104      
#define BOARD_SIZE 9

typedef struct {
    char board[BOARD_SIZE];
    int turn;       // 현재 차례: 0 또는 1
    int game_over;  // 게임 종료 여부
    int winner;     // 승자: -1(무승부), 0 또는 1
    int client_count;
    int ready[2];
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} SharedMemory;

SharedMemory* shared_mem;
int player_id;

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

void* input_thread(void* arg) {
    while (!shared_mem->game_over) {
        pthread_mutex_lock(&shared_mem->mutex);

        // 자신의 차례인지 확인
        while (shared_mem->turn != player_id && !shared_mem->game_over) {
            pthread_cond_wait(&shared_mem->cond, &shared_mem->mutex);
        }

        if (shared_mem->game_over) {
            pthread_mutex_unlock(&shared_mem->mutex);
            break;
        }

        // 현재 보드 상태 출력
        system("clear");
        printf("플레이어 %d의 차례입니다.\n", player_id);
        print_board();

        // 사용자 입력 받기
        int pos;
        printf("위치 선택 (1-9): ");
        if (scanf("%d", &pos) != 1) {
            printf("유효한 숫자를 입력하세요.\n");
            // 입력 버퍼 클리어
            while (getchar() != '\n');
            pthread_mutex_unlock(&shared_mem->mutex);
            continue;
        }
        pos--;

        if (pos < 0 || pos >= BOARD_SIZE || shared_mem->board[pos] != ' ') {
            printf("잘못된 위치입니다. 다시 시도하세요.\n");
        }
        else {
            shared_mem->board[pos] = (player_id == 0) ? 'X' : 'O';
            shared_mem->turn = (player_id + 1) % 2; // 턴 전환
            pthread_cond_broadcast(&shared_mem->cond); // 다른 플레이어에게 알림
        }

        pthread_mutex_unlock(&shared_mem->mutex);
        usleep(100000); // 0.1초 대기
    }
    return NULL;
}

void* update_thread(void* arg) {
    while (!shared_mem->game_over) {
        pthread_mutex_lock(&shared_mem->mutex);

        // 자신의 차례가 아닐 때 보드 업데이트 확인
        if (shared_mem->turn != player_id) {
            system("clear");
            printf("상대방의 차례입니다. 대기 중...\n");
            print_board();
        }

        pthread_mutex_unlock(&shared_mem->mutex);
        sleep(1);
    }
    return NULL;
}



int main() {
    int shm_id = shmget(SHM_KEY, sizeof(SharedMemory), 0666);

    if (shm_id < 0) {
        perror("shmget failed");
        exit(1);
    }

    shared_mem = (SharedMemory*)shmat(shm_id, NULL, 0);

    if (shared_mem == (void*)-1) {
        perror("shmat failed");
        exit(1);
    }

    // 뮤텍스와 조건 변수를 초기화하지 않습니다.
    // 클라이언트는 서버에서 초기화한 뮤텍스와 조건 변수를 사용합니다.

    pthread_mutex_lock(&shared_mem->mutex);

    // 클라이언트 수 체크 및 플레이어 ID 할당
    if (shared_mem->client_count >= 2) {
        printf("이미 최대 클라이언트 수에 도달했습니다.\n");
        pthread_mutex_unlock(&shared_mem->mutex);
        shmdt(shared_mem);
        exit(1);
    }

    player_id = shared_mem->client_count;
    shared_mem->client_count++;
    shared_mem->ready[player_id] = 1;
    pthread_cond_broadcast(&shared_mem->cond); // 서버에 알림

    pthread_mutex_unlock(&shared_mem->mutex);

    // 스레드 생성
    pthread_t input_t, update_t;
    pthread_create(&input_t, NULL, input_thread, NULL);
    pthread_create(&update_t, NULL, update_thread, NULL);

    // 스레드 종료 대기
    pthread_join(input_t, NULL);
    pthread_join(update_t, NULL);

    // 게임 결과 출력
    pthread_mutex_lock(&shared_mem->mutex);
    system("clear");
    print_board();
    if (shared_mem->winner == -1) {
        printf("게임 결과: 무승부입니다!\n");
    }
    else if (shared_mem->winner == player_id) {
        printf("게임 결과: 당신이 승리했습니다!\n");
    }
    else {
        printf("게임 결과: 당신이 패배했습니다.\n");
    }
    pthread_mutex_unlock(&shared_mem->mutex);

    // 공유 메모리 분리
    shmdt(shared_mem);

    return 0;
}

