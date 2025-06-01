#include <assert.h>
#include <mpi.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
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

  const char *pkt_fmt = "{src=%d ts=%d},";
  for (int i = 0; i < len; i++) {
    int needed = snprintf(NULL, 0, pkt_fmt, (queue + i)->src, (queue + i)->ts);
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
    index += sprintf(out + index, pkt_fmt, (queue + i)->src, (queue + i)->ts);
  }

  // Remove the trailing comma if the list is not empty
  if (len > 0 && index > 0) {
    out[index - 1] = '\0';
  }
}

void debug(const char *message, ...) {
  va_list vl;
  va_start(vl, message);
  char *msg;
  if (0 > vasprintf(&msg, message, vl))
    return; // ALLOC ERROR
  va_end(vl);

  const char *role =
      is_babcia ? "Babcia" : (is_studentka ? "Studentka" : "Proces");
  const int required_ack = is_babcia ? B - 1 : S - 1;

  char *out_queue = NULL;
  list_to_str(deffered_queue, deferred_queue_size, &out_queue);
  if (csv_mode) {
    printf("%d;%d;\"%s\";\"%s\";%d;%d;%d;%d;\"%s\";%d;%d;%d\n", rank,
           clockLamport, role, msg, liczba_sloikow, liczba_konfitur, has_jar,
           has_jam, out_queue, ack_count, required_ack, in_cs);

  } else {
    printf("[%d][%d][%s] %s [sloiki: %d, konfitury: %d, has_jar: %d, has_jam: "
           "%d, ACK: %d/%d, inCS: %d, q: %s]\n",
           rank, clockLamport, role, msg, liczba_sloikow, liczba_konfitur,
           has_jar, has_jam, ack_count, required_ack, in_cs, out_queue);
    fflush(stdout);
  }
  free(out_queue);
  free(msg);
}

void inc_clock(int received_ts) {
  clockLamport = (clockLamport > received_ts ? clockLamport : received_ts) + 1;
}

bool receive_condition() {
  debug("Sprawdzam czy mogę zabrać");
  bool all_ack_received;
  bool resources_available;
  if (is_babcia) {
    all_ack_received = ack_count == B - 1;
    resources_available = liczba_sloikow > 0;
  } else { // studentka
    all_ack_received = ack_count == S - 1;
    resources_available = liczba_konfitur > 0;
  }

  const bool res = !(all_ack_received && resources_available);
  debug("Sprawdziłam: %d", res);
  return res;
}

void send_packet(int dst, int tag) {
  packet_t pkt = {.ts = clockLamport, .src = rank, .type = tag};
  MPI_Send(&pkt, 1, MPI_PACKET_T, dst, tag, MPI_COMM_WORLD);
  debug("Wysyłam %s do %d", tag_status_disp(tag), dst);
}

int compare_packet(const void *a, const void *b) {
  packet_t *pa = (packet_t *)a;
  packet_t *pb = (packet_t *)b;
  if (pa->ts != pb->ts)
    return pa->ts - pb->ts;
  return pa->src - pb->src;
}

void add_to_queue(packet_t pkt) {
  debug("Dodaję do kolejki {src=%d, ts=%d}", pkt.src, pkt.ts);
  if (deferred_queue_size >= B + S) {
    debug("PEŁNA KOLEJKA");
    return;
  }
  deffered_queue[deferred_queue_size++] = pkt;
  qsort(deffered_queue, deferred_queue_size, sizeof(packet_t), compare_packet);
}

void remove_from_queue(int src) {
  debug("Usuwam z kolejki src=%d", src);
  for (int i = 0; i < deferred_queue_size; i++) {
    if (deffered_queue[i].src == src) {
      debug("Znalazłam i usuwam {src=%d, ts=%d}", deffered_queue[i].src,
            deffered_queue[i].ts);
      for (int j = i; j < deferred_queue_size - 1; j++) {
        deffered_queue[j] = deffered_queue[j + 1];
      }
      deferred_queue_size--;
      return;
    }
  }
  debug("src=%d NIE ISTNIEJE W KOLEJCE", src);
}

int find_in_queue(int src) {
  debug("Szukam w kolejce src=%d", src);
  for (int i = 0; i < deferred_queue_size; i++) {
    if (deffered_queue[i].src == src) {
      debug("Znalazłam w kolejce: i=%d -> {src=%d, ts=%d}", i,
            deffered_queue[i].src, deffered_queue[i].ts);
      return i;
    }
  }
  debug("Brak src=%d w kolejce", src);
  return -1;
}
bool has_priority(int p2) {
  int my_pos_in_q = find_in_queue(rank);
  int their_pos_in_q = find_in_queue(p2);
  debug("Jestem %d w kolejce, a src=%d na miejscu %d", my_pos_in_q, p2,
        their_pos_in_q);
  if (their_pos_in_q < 0) {
    debug("Osoba (src=%d) jest na miejscu -1", p2);
    return false;
  }
  if (my_pos_in_q < 0) {
    return false;
  }

  return my_pos_in_q < their_pos_in_q;
}

void *receive_thread_func(void *arg) {
  packet_t pkt;
  MPI_Status status;

  while (true) {
    MPI_Recv(&pkt, 1, MPI_PACKET_T, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD,
             &status);

    pthread_mutex_lock(&mutex);
    inc_clock(pkt.ts);

    const char *tag_disp = tag_status_disp(status.MPI_TAG);
    debug("Otrzymałam %s od [%d]", tag_disp, pkt.src);

    switch (status.MPI_TAG) {
    case TAG_REQ:
      if ((is_babcia && pkt.src < B) ||
          (is_studentka && pkt.src >= B && pkt.src < B + S)) {

        add_to_queue(pkt);
        // Najpierw trzeba sprawdzić czy nie jesteśmy obecnie w sekcji
        // krytycznej
        debug("Czy jestem w sekcji krytycznej: %d", in_cs);
        if (in_cs || (has_priority(pkt.src))) {
          // jeśli tak to zapisujemy to do kolejki
          debug("Mam priorytet, odeślę wiadomość potem");
        } else {
          // W przeciwnym razie wysyłamy odpowiedź
          send_packet(pkt.src, TAG_ACK);
          remove_from_queue(pkt.src);
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
      // remove_from_queue(pkt.src);
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
    debug("przetworzyłam %s od [%d]", tag_disp, pkt.src);

    if (!receive_condition()) {
      pthread_cond_signal(&cond);
    }

    pthread_mutex_unlock(&mutex);
  }

  return NULL;
}

void wait_until_can_proceed() {
  debug("enter wait_until_can_proceed");
  pthread_mutex_lock(&mutex);
  while (receive_condition()) {
    pthread_cond_wait(&cond, &mutex);
  }
  in_cs = true;
  remove_from_queue(rank);
  pthread_mutex_unlock(&mutex);
  debug("exit wait_until_can_proceed");
}

void request_resource() {
  debug("enter request_resource");
  pthread_mutex_lock(&mutex);
  clockLamport++;
  memset(waiting_ack, 0, (B + S) * sizeof(bool));
  ack_count = 0;
  packet_t pkt = {.ts = clockLamport, .src = rank, .type = TAG_REQ};
  add_to_queue(pkt);

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
  debug("enter enter_critical_section");
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

  // wysyła opóźnione potwierdzenia wejścia do sekcji krytycznej
  for (int i = 0; i < deferred_queue_size; i++) {
    int send_to = deffered_queue[i].src;
    if (send_to == rank)
      continue;
    if ((is_babcia && send_to >= B) || (is_studentka && send_to < B))
      continue;

    send_packet(send_to, TAG_ACK);
  }
  debug("Wysyłam zaległe ACK");
  deferred_queue_size = 0;

  for (int i = 0; i < size; i++) {
    if (i != rank)
      send_packet(i, TAG_REL);
  }

  debug("Wysłałam REL do wszystkich");
  in_cs = false;
  pthread_mutex_unlock(&mutex);
  debug("exit enter_critical_section");
}

void run_process() {
  while (true) {
    if (is_babcia) {
      if (!has_jar) {
        debug("Chcę zabrać słoik");
        request_resource();
        wait_until_can_proceed();
        enter_critical_section();
        debug("Mam słoik");
      } else {
        debug("Rozpoczynam produkcję konfitury");
        sleep(rand() % 6 + 1);
        pthread_mutex_lock(&mutex);
        has_jar = false;
        has_jam = true;
        liczba_konfitur++;
        debug("Mam konfiturę");
        pthread_mutex_unlock(&mutex);
        sleep(rand() % 13 + 1);
        pthread_mutex_lock(&mutex);
        // Babcia wysyła do każdej studentki, że pojawiła się nowa konfitura
        // dopiero jak chce się jej pozbyć (konfitury)
        for (int i = B; i < B + S; i++) {
          send_packet(i, TAG_FULL);
        }
        has_jam = false;
        pthread_mutex_unlock(&mutex);
        debug("Oddałam konfiturę");
      }
    }

    if (is_studentka) {
      if (!has_jam) {
        debug("Chcę zabrać konfiturę");
        request_resource();
        wait_until_can_proceed();
        enter_critical_section();
        debug("Mam konfiturę");
        sleep(rand() % 8 + 1);
        pthread_mutex_lock(&mutex);
        has_jam = false;
        has_jar = true;
        liczba_sloikow++;
        debug("Mam słoik");
        pthread_mutex_unlock(&mutex);
      } else {
        sleep(rand() % 10 + 1);
        debug("Chcę oddać słoik");
        pthread_mutex_lock(&mutex);
        // Studentka wysyła do każdej babci, że zwolnił się nowy słoik
        // dopiero jak chce się go pozbyć
        for (int i = 0; i < B; i++) {
          send_packet(i, TAG_EMPTY);
        }
        has_jar = false;
        pthread_mutex_unlock(&mutex);
        debug("Oddałam słoik");
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
    printf("rank;clock;proc_type;message;sloiki;konfitury;has_jar;has_jam;"
           "queue;recv_ack;needed_ack;in_cs\n");
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
  if (waiting_ack == 0 || deffered_queue == 0) {
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
