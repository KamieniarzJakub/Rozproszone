#include <assert.h>
#include <mpi.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

pthread_t receiver_thread;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
bool in_cs = false;

// Rodzaje wiadomości
enum MESSAGES {
  // Żądanie pobranie zasobu (studentka - konfitury, babcia - słoika)
  TAG_REQ = 1,
  // Potwierdzenie i zezwolenie na zabranie zasobu przez inny proces
  TAG_ACK = 2,
  TAG_REL = 3,   // Opuszczenie sekcji krytycznej
  TAG_EMPTY = 5, // Nowy pusty słoik
  TAG_FULL = 6   // Nowa konfitura
};

typedef struct {
  int ts;   // zegar Lamporta
  int src;  // od kogo wysłane
  int type; // jaki rodzaj (enum MESSAGES)
} packet_t;

MPI_Datatype MPI_PACKET_T;

int P = 0; // maksymalna liczba sloikow
int K = 0; // maksymalna liczba konfitur
int B = 3; // liczba babć
int S = 4; // liczba studentek
bool csv_mode = false;

int clockLamport = 0;
int rank, size;
bool *waiting_ack = NULL; // Tablica o rozmiarze procesów tego samego typu co
                          // obecny przechowująca odebrane potwierdzenia od
                          // danego procesu, indeks w tablicy odpowiada tid
int ack_count = 0;

bool has_jar = false;
bool has_jam = false;
bool is_babcia = false;
bool is_studentka = false;
int liczba_sloikow = 0;
int liczba_konfitur = 0;

packet_t *deffered_queue = NULL;
size_t deferred_queue_size = 0;

void inc_clock(int received_ts) {
  clockLamport = (clockLamport > received_ts ? clockLamport : received_ts) + 1;
}

bool receive_condition() {
  bool all_ack_received;
  bool resources_available;
  if (is_babcia) {
    all_ack_received = ack_count == B - 1;
    resources_available = liczba_sloikow > 0;
  } else { // studentka
    all_ack_received = ack_count == S - 1;
    resources_available = liczba_konfitur > 0;
  }

  return !(all_ack_received && resources_available);
}

// void print_queues(packet_t queue[], size_t queue_size) {
//   printf("[Rank %d][Clock %d] [kolejka sloiki]: ", rank, clockLamport);
//   for (int i = 0; i < queue_sloiki_size; i++) {
//     printf("%d(ts=%d) ", queue_sloiki[i].src, queue_sloiki[i].ts);
//   }
//   printf("| [kolejka konfitury]: ");
//   for (int i = 0; i < queue_konfitury_size; i++) {
//     printf("%d(ts=%d) ", queue_konfitury[i].src, queue_konfitury[i].ts);
//   }
//   printf("\n");
//   fflush(stdout);
// }

void list_to_str(packet_t *queue, int len, char **out_ptr) {
  int current_len = 1; // space for the null terminator
  *out_ptr = (char *)malloc(current_len * sizeof(char));
  if (*out_ptr == NULL) {
    fprintf(stderr, "MALLOC ERROR\n");
    exit(1);
  }
  (*out_ptr)[0] = '\0';

  char *out = *out_ptr;
  int index = 0;

  for (int i = 0; i < len; i++) {
    int needed =
        snprintf(NULL, 0, "%d->%d,", (queue + i)->src, (queue + i)->ts);
    if (index + needed >= current_len) {
      current_len += needed + 1; // +1 for next comma and null terminator
      char *temp = (char *)realloc(out, current_len * sizeof(char));
      if (temp == NULL) {
        fprintf(stderr, "REALLOC ERROR\n");
        free(out);
        exit(1);
      }
      out = temp;
      *out_ptr = out;
    }
    index += sprintf(out + index, "%d->%d,", (queue + i)->src, (queue + i)->ts);
  }

  // Remove the trailing comma if the list is not empty
  if (len > 0 && index > 0) {
    out[index - 1] = '\0';
  }
}
void debug(const char *message) {
  const char *role =
      is_babcia ? "Babcia" : (is_studentka ? "Studentka" : "Proces");
  const int required_ack = is_babcia ? B - 1 : S - 1;

  if (csv_mode) {
    char *out_queue = NULL;
    list_to_str(deffered_queue, deferred_queue_size, &out_queue);

    printf("%d;%d;%s;\"%s\";%d;%d;%d;%d;\"%s\";%d;%d\n", rank, clockLamport,
           role, message, liczba_sloikow, liczba_konfitur, has_jar, has_jam,
           out_queue, ack_count, required_ack);

    free(out_queue);
  } else {
    printf("[%d][%d][%s] %s [sloiki: %d, konfitury: %d, has_jar: %d, has_jam: "
           "%d, ACK: %d/%d]\n",
           rank, clockLamport, role, message, liczba_sloikow, liczba_konfitur,
           has_jar, has_jam, ack_count, required_ack);
    // print_queues();
    fflush(stdout);
  }
}

void send_packet(int dst, int tag) {
  packet_t pkt = {.ts = clockLamport, .src = rank, .type = tag};
  MPI_Send(&pkt, 1, MPI_PACKET_T, dst, tag, MPI_COMM_WORLD);
}

int compare_packet(const void *a, const void *b) {
  packet_t *pa = (packet_t *)a;
  packet_t *pb = (packet_t *)b;
  if (pa->ts != pb->ts)
    return pa->ts - pb->ts;
  return pa->src - pb->src;
}

void add_to_queue(packet_t pkt) {
  if (deferred_queue_size >= B + S) {
    fprintf(stderr, "TRYING TO ADD MORE PACKETS TO QUEUE THAN ALLOWED");
    return;
  }
  deffered_queue[deferred_queue_size++] = pkt;
  qsort(deffered_queue, deferred_queue_size, sizeof(packet_t), compare_packet);
}

void remove_from_queue(int src) {
  for (int i = 0; i < deferred_queue_size; i++) {
    if (deffered_queue[i].src == src) {
      for (int j = i; j < deferred_queue_size - 1; j++) {
        deffered_queue[j] = deffered_queue[j + 1];
      }
      deferred_queue_size--;
      break;
    }
  }
  fprintf(stderr, "TRYING TO REMOVE NONEXISTENT ID: %d", src);
}

const char *tag_status_disp(int tag) {
  switch (tag) {
  case TAG_REQ:
    return "REQ";
  case TAG_ACK:
    return "ACK";
  case TAG_REL:
    return "REL";
  case TAG_EMPTY:
    return "EMPTY";
  case TAG_FULL:
    return "FULL";
  default:
    return "INNE";
  }
}

void *receive_thread_func(void *arg) {
  packet_t pkt;
  MPI_Status status;

  while (true) {
    MPI_Recv(&pkt, 1, MPI_PACKET_T, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD,
             &status);

    pthread_mutex_lock(&mutex);
    inc_clock(pkt.ts);

    char buf[128];
    snprintf(buf, sizeof(buf), "Otrzymałam %s od [%d]",
             tag_status_disp(status.MPI_TAG), pkt.src);

    switch (status.MPI_TAG) {
    case TAG_REQ:
      if ((is_babcia && pkt.src < B) ||
          (is_studentka && pkt.src >= B && pkt.src < B + S)) {

        // Najpierw trzeba sprawdzić czy nie jesteśmy obecnie w sekcji
        // krytycznej
        if (in_cs) {
          // jeśli tak to zapisujemy to do kolejki
          add_to_queue(pkt);
        } else {
          // W przeciwnym razie wysyłamy odpowiedź
          send_packet(pkt.src, TAG_ACK);
        }
      }
      break;
    case TAG_ACK:
      if (!waiting_ack[pkt.src]) {
        ack_count++;
        waiting_ack[pkt.src] = true;
      }
      break;
    case TAG_REL:
      remove_from_queue(pkt.src);
      if (pkt.src < B) {
        liczba_sloikow--;
      } else {
        liczba_konfitur--;
      }
      break;
    case TAG_EMPTY:
      liczba_sloikow++;
      break;
    case TAG_FULL:
      liczba_konfitur++;
      break;
    }
    debug(buf);

    if (!receive_condition()) {
      pthread_cond_signal(&cond);
    }

    pthread_mutex_unlock(&mutex);
  }

  return NULL;
}

void wait_until_can_proceed() {
  pthread_mutex_lock(&mutex);
  while (receive_condition()) {
    pthread_cond_wait(&cond, &mutex);
  }
  in_cs = true;
  pthread_mutex_unlock(&mutex);
}

void request_resource() {
  pthread_mutex_lock(&mutex);
  clockLamport++;
  memset(waiting_ack, 0, (B + S) * sizeof(bool));
  ack_count = 0;
  packet_t pkt = {.ts = clockLamport, .src = rank, .type = TAG_REQ};
  // add_to_queue(pkt);

  if (is_babcia) {
    for (int i = 0; i < rank; i++)
      send_packet(i, TAG_REQ);
    for (int i = rank + 1; i < B; i++)
      send_packet(i, TAG_REQ);
  } else if (is_studentka) {
    for (int i = B; i < rank; i++)
      send_packet(i, TAG_REQ);
    for (int i = rank + 1; i < B + S; i++)
      send_packet(i, TAG_REQ);
  }

  debug(is_babcia ? "Wysyłam prośbę o słoik" : "Wysyłam prośbę o konfiturę");
  pthread_mutex_unlock(&mutex);
}

void enter_critical_section() {
  sleep(rand() % 2 + 1);

  pthread_mutex_lock(&mutex);
  debug("Wchodzę do sekcji krytycznej");

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

  if (is_babcia) {
    // Babcia wysyła opóźnione potwierdzenia wejścia do sekcji krytycznej
    // TODO
  } else {
    // Studentka wysyła opóźnione potwierdzenia wejścia do sekcji krytycznej
    // TODO
  }

  // TODO: Instead of broadcast to all the process should ONLY send messages to:
  //  - TAG_ACK message to all the deferred requests.
  for (int i = 0; i < size; i++) {
    if (i != rank)
      send_packet(i, TAG_REL);
  }

  // remove_from_queue(rank);
  debug("Wysyłam REL do wszystkich, wychodzę z krytycznej");
  in_cs = false;
  pthread_mutex_unlock(&mutex);
}

void run_process() {
  while (true) {
    if (is_babcia) {
      if (!has_jar && !has_jam) {
        request_resource();
        wait_until_can_proceed();
        enter_critical_section();
      } else if (has_jar && !has_jam) {
        debug("Rozpoczynam produkcję konfitury");
        sleep(rand() % 6 + 1);
        pthread_mutex_lock(&mutex);
        has_jar = false;
        has_jam = true;
        liczba_konfitur++;
        // Babcia wysyła do każdej studentki, że pojawiła się nowa konfitura
        for (int i = B; i < B + S; i++) {
          send_packet(i, TAG_FULL);
        }
        debug("Wysłałam FULL, mam konfiturę");
        pthread_mutex_unlock(&mutex);
      } else if (has_jam) {
        sleep(rand() % 13 + 1);
        pthread_mutex_lock(&mutex);
        has_jam = false;
        pthread_mutex_unlock(&mutex);
      }
    }

    if (is_studentka) {
      if (!has_jam && !has_jar) {
        request_resource();
        wait_until_can_proceed();
        enter_critical_section();
      } else if (has_jam && !has_jar) {
        debug("Zjadam konfiturę");
        sleep(rand() % 8 + 1);
        pthread_mutex_lock(&mutex);
        has_jam = false;
        has_jar = true;
        liczba_sloikow++;
        // Studentka wysyła do każdej babci, że zwolnił się nowy słoik
        for (int i = 0; i < B; i++) {
          send_packet(i, TAG_EMPTY);
        }
        debug("Wysłałam EMPTY, oddałam słoik");
        pthread_mutex_unlock(&mutex);
      } else if (has_jar) {
        sleep(rand() % 10 + 1);
        pthread_mutex_lock(&mutex);
        has_jar = false;
        pthread_mutex_unlock(&mutex);
      }
    }

    sleep(1);
  }
}

void init_packet_type() {
  const int count = 3;
  int lengths[] = {1, 1, 1};
  MPI_Aint offsets[] = {offsetof(packet_t, ts), offsetof(packet_t, src),
                        offsetof(packet_t, type)};
  MPI_Datatype types[] = {MPI_INT, MPI_INT, MPI_INT};
  MPI_Type_create_struct(count, lengths, offsets, types, &MPI_PACKET_T);
  MPI_Type_commit(&MPI_PACKET_T);
}

void finalize(int signo) {
  MPI_Type_free(&MPI_PACKET_T);
  MPI_Finalize();
  free(waiting_ack);
  free(deffered_queue);
}

int main(int argc, char **argv) {
  // Inicjalizacja MPI
  MPI_Init(&argc, &argv);
  MPI_Comm_size(MPI_COMM_WORLD, &size);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  signal(SIGINT, finalize);
  signal(SIGCHLD, finalize);
  signal(SIGKILL, finalize);

  // Parsowanie parametrów wejściowych (liczba babć, liczba słoików, czy
  // zapisywać jako csv)
  if (argc < 2) {
    if (rank == 0)
      fprintf(stderr, "Użycie: %s <ile_babci> [liczba_sloikow] [csv]\n",
              argv[0]);
    MPI_Abort(MPI_COMM_WORLD, 1);
    return 1;
  }

  B = atoi(argv[1]);
  if (B < 1 || B >= size) {
    if (rank == 0)
      fprintf(stderr, "Błędna liczba babć\n");
    MPI_Abort(MPI_COMM_WORLD, 2);
    return 2;
  }

  if (argc >= 3) {
    P = atoi(argv[2]);
    if (P < 1) {
      if (rank == 0)
        fprintf(stderr, "Liczba słoików musi być > 0\n");
      MPI_Abort(MPI_COMM_WORLD, 3);
      return 3;
    }
  } else {
    P = B;
  }

  S = size - B;
  K = P;
  liczba_sloikow = P;
  if (argc >= 4 && atoi(argv[3]))
    csv_mode = true;

  if (csv_mode && rank == 0) {
    printf("rank;clock;proc_type;message;sloiki;konfitury;has_jar;has_jam;jar_"
           "queue;jam_queue;recv_ack;needed_ack\n");
  }

  srand(time(NULL) + rank);
  init_packet_type();
  is_babcia = (rank < B);
  is_studentka = (rank >= B && rank < B + S);
  if (is_babcia) {
    waiting_ack = malloc((B + S) * sizeof(bool));
    deffered_queue = malloc((B + S) * sizeof(bool));
  } else {
    waiting_ack = malloc((B + S) * sizeof(bool));
    deffered_queue = malloc((B + S) * sizeof(bool));
  }
  if (waiting_ack == 0) {
    fprintf(stderr, "MALLOC FAILED");
    exit(1);
  }

  debug("Start procesu");

  // Stworzenie dodatkowego procesu do komunikacji
  pthread_create(&receiver_thread, NULL, receive_thread_func, NULL);

  // Główny proces przetwarzania
  run_process();

  return 0;
}
