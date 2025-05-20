#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#define P 3 // maksymalna liczba sloikow
#define K 3 // maksymalna liczba konfitur
#define ROOT 0

#define TAG_REQ 1
#define TAG_ACK 2
#define TAG_REL 3
#define TAG_EVENT 4
#define TAG_EMPTY 5
#define TAG_FULL 6

#define TRUE 1
#define MAX_PROCESSES 32

int clockLamport = 0;
int rank, size;
bool waiting_ack[MAX_PROCESSES];
int ack_count = 0;
bool has_jar = false;
bool has_jam = false;
bool is_babcia = false;
bool is_studentka = false;
int liczba_sloikow = P;
int liczba_konfitur = 0;

int B = 3; // liczba babć
int S = 1; // liczba studentek

typedef struct {
    int ts;
    int src;
    int type;
} packet_t;

MPI_Datatype MPI_PACKET_T;

// Oddzielne kolejki
packet_t queue_sloiki[MAX_PROCESSES];
int queue_sloiki_size = 0;

packet_t queue_konfitury[MAX_PROCESSES];
int queue_konfitury_size = 0;

void inc_clock(int received_ts) {
    clockLamport = (clockLamport > received_ts ? clockLamport : received_ts) + 1;
}

void print_queue() {
    printf("[Rank %d][Clock %d] [kolejka sloiki]: ", rank, clockLamport);
    for (int i = 0; i < queue_sloiki_size; i++) {
        printf("%d(ts=%d) ", queue_sloiki[i].src, queue_sloiki[i].ts);
    }
    printf("| [kolejka konfitury]: ");
    for (int i = 0; i < queue_konfitury_size; i++) {
        printf("%d(ts=%d) ", queue_konfitury[i].src, queue_konfitury[i].ts);
    }
    printf("\n");
    fflush(stdout);
}

void debug(const char *message) {
    const char *role = is_babcia ? "Babcia" : (is_studentka ? "Studentka" : "Proces");
    printf("[%d][%d][%s] %s [sloiki: %d, konfitury: %d, has_jar: %d, has_jam: %d, ACK: %d/%d]\n", 
           rank, clockLamport, role, message, liczba_sloikow, liczba_konfitur, has_jar, has_jam, ack_count, is_babcia ? B - 1 : S - 1);
    print_queue();
    fflush(stdout);
}

void send_packet(int dst, int tag) {
    packet_t pkt = {.ts = clockLamport, .src = rank, .type = tag};
    MPI_Send(&pkt, 1, MPI_PACKET_T, dst, tag, MPI_COMM_WORLD);
}

void broadcast_packet(int tag) {
    for (int i = 0; i < size; i++) {
        if (i != rank)
            send_packet(i, tag);
    }
}

int compare_packet(const void *a, const void *b) {
    packet_t *pa = (packet_t *)a;
    packet_t *pb = (packet_t *)b;
    if (pa->ts != pb->ts)
        return pa->ts - pb->ts;
    return pa->src - pb->src;
}

void add_to_queue(packet_t pkt) {
    if (is_babcia || pkt.src < B) {
        queue_sloiki[queue_sloiki_size++] = pkt;
        qsort(queue_sloiki, queue_sloiki_size, sizeof(packet_t), compare_packet);
    } else {
        queue_konfitury[queue_konfitury_size++] = pkt;
        qsort(queue_konfitury, queue_konfitury_size, sizeof(packet_t), compare_packet);
    }
}

bool is_first_in_queue() {
    if (is_babcia) {
        return queue_sloiki_size > 0 && queue_sloiki[0].src == rank;
    } else {
        return queue_konfitury_size > 0 && queue_konfitury[0].src == rank;
    }
}

void remove_from_queue(int src) {
    int *size = is_babcia ? &queue_sloiki_size : &queue_konfitury_size;
    packet_t *queue = is_babcia ? queue_sloiki : queue_konfitury;
    for (int i = 0; i < *size; i++) {
        if (queue[i].src == src) {
            for (int j = i; j < *size - 1; j++) {
                queue[j] = queue[j + 1];
            }
            (*size)--;
            break;
        }
    }
}

void request_resource() {
    clockLamport++;
    memset(waiting_ack, 0, sizeof(waiting_ack));
    ack_count = 0;
    packet_t pkt = {.ts = clockLamport, .src = rank, .type = TAG_REQ};
    add_to_queue(pkt);
    for (int i = 0; i < size; i++) {
        if ((is_babcia && i < B && i != rank) || (is_studentka && i >= B && i < B + S && i != rank)) {
            send_packet(i, TAG_REQ);
        }
    }
    debug(is_babcia ? "Wysyłam prośbę o słoik" : "Wysyłam prośbę o konfiturę");
}

void receive_loop() {
    packet_t pkt;
    MPI_Status status;

    while (!(ack_count == (is_babcia ? B - 1 : S - 1) && is_first_in_queue() && ((is_babcia && liczba_sloikow > 0) || (is_studentka && liczba_konfitur > 0)))) {
        MPI_Recv(&pkt, 1, MPI_PACKET_T, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        inc_clock(pkt.ts);

        char buf[128];
        snprintf(buf, sizeof(buf), "Otrzymałam %s od [%d]", 
                 status.MPI_TAG == TAG_REQ ? "REQ" : 
                 status.MPI_TAG == TAG_ACK ? "ACK" : 
                 status.MPI_TAG == TAG_REL ? "REL" : 
                 status.MPI_TAG == TAG_EMPTY ? "EMPTY" : 
                 status.MPI_TAG == TAG_FULL ? "FULL" : "INNE", 
                 pkt.src);

        switch (status.MPI_TAG) {
            case TAG_REQ:
                if ((is_babcia && pkt.src < B) || (is_studentka && pkt.src >= B && pkt.src < B + S)) {
                    add_to_queue(pkt);
                    send_packet(pkt.src, TAG_ACK);
                    debug(buf);
                }
                break;
            case TAG_ACK:
                if (!waiting_ack[pkt.src]) {
                    ack_count++;
                    waiting_ack[pkt.src] = true;
                    debug(buf);
                }
                break;
            case TAG_REL:
                remove_from_queue(pkt.src);
                if (pkt.src < B) { // babcia
                    liczba_sloikow--;
                } else { // studentka
                    liczba_konfitur--;
                }
                debug(buf);
                break;
            case TAG_EMPTY:
                liczba_sloikow++;
                debug(buf);
                break;
            case TAG_FULL:
                liczba_konfitur++;
                debug(buf);
                break;
        }
    }
}

void enter_critical_section() {
    debug("Wchodzę do sekcji krytycznej");
    //sleep(rand() % 2 + 1);
    clockLamport++;

    if (is_babcia) {
        liczba_sloikow--;
        has_jar = true;
        debug("Zabieram słoik");
    } else {
        liczba_konfitur--;
        has_jam = true;
        debug("Zabieram konfiturę");
    }

    clockLamport++;
    broadcast_packet(TAG_REL);
    remove_from_queue(rank);
    debug("Wysyłam REL do wszystkich (zabrałam to co chciałam i wychodzę z krytycznej)");
}

void run_process() {
    while (TRUE) {
        if (is_babcia) {
            if (!has_jar && !has_jam) {
                request_resource();
                receive_loop();
                if (ack_count == B - 1 && is_first_in_queue() && liczba_sloikow > 0) {
                    enter_critical_section();
                    has_jar = true;
                }
            } else if (has_jar && !has_jam) {
                debug("Rozpoczynam produkcję konfitury");
                sleep(rand() % 6 + 1);
                has_jar = false;
                has_jam = true;
                liczba_konfitur++;
                broadcast_packet(TAG_FULL);
                debug("Wysłałam FULL, mam konfiturę");
            } else if (has_jam) {
                sleep(rand() % 13 + 1);
                has_jam = false;
            }
        }

        if (is_studentka) {
            if (!has_jam && !has_jar) {
                request_resource();
                receive_loop();
                if (ack_count == S - 1 && is_first_in_queue() && liczba_konfitur > 0) {
                    enter_critical_section();
                    has_jam = true;
                }
            } else if (has_jam && !has_jar) {
                debug("Zjadam konfiturę");
                sleep(rand() % 8 + 1);
                has_jam = false;
                has_jar = true;
                liczba_sloikow++;
                broadcast_packet(TAG_EMPTY);
                debug("Wysłałam EMPTY, oddałam słoik");
            } else if (has_jar) {
                sleep(rand() % 10 + 1);
                has_jar = false;
            }
        }

        sleep(1);
    }
}

void init_packet_type() {
    const int count = 3;
    int lengths[] = {1, 1, 1};
    MPI_Aint offsets[] = {
        offsetof(packet_t, ts),
        offsetof(packet_t, src),
        offsetof(packet_t, type)
    };
    MPI_Datatype types[] = {MPI_INT, MPI_INT, MPI_INT};
    MPI_Type_create_struct(count, lengths, offsets, types, &MPI_PACKET_T);
    MPI_Type_commit(&MPI_PACKET_T);
}

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    srand(time(NULL) + rank);
    init_packet_type();

    is_babcia = (rank < B);
    is_studentka = (rank >= B && rank < B + S);

    debug("Start procesu");

    run_process();

    MPI_Type_free(&MPI_PACKET_T);
    MPI_Finalize();
    return 0;
}
