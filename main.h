#include <mpi.h>
#define BABCI_COUNT 2
#define STUDENTKI_COUNT 2
#define ROOT 0

#define APP_PKT 1
#define ACK_PKT 2
#define REQ_PKT 3
#define REL_PKT 4

#define TRUE 1

typedef struct {
    int ts;
    int src;
    int data;
} packet_t;

MPI_Datatype MPI_PAKIET_T;

enum zasob { R_SLOIK, R_KONFITURA };
enum event { E_KONFITURA_GOTOWA, E_SLOIK_ZWOLNIONY };

void inicjuj_typ_pakietu();
void request_resource(enum zasob, int, int);
void release_resource(enum zasob, int, int);
void broadcast_event(enum event);
void run_babcia(int rank, int size);
void run_studentka(int rank, int size);
void debug(const char *fmt, ...);
