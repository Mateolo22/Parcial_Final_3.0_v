#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "kernel.h"
#include "seguridad.h"
#include "terminal.h"
#include "fs.h"
#include "juego.h"

#define ANCHO 20
#define ALTO 10
#define MAX_COLA 200
#define MAGIC_PARTIDA 0x534E414B
#define VERSION_PARTIDA 1
#define INTERVALO_AUTOGUARDADO 15

typedef struct { int x, y; } Punto;

typedef struct {
    unsigned int magic;
    unsigned int version;
    char jugador[32];
    int puntaje;
    int nivel;
    int direccion;
    int largo;
    int activo;
    Punto cuerpo[MAX_COLA];
    Punto comida;
    long guardada_en;
} Partida;

static Partida partida;
static pthread_mutex_t mutex_partida = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mutex_control = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mutex_guardado = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond_autoguardado = PTHREAD_COND_INITIALIZER;
static int detener_autoguardado = 0;
static struct termios termios_original;
static int termios_disponible = 0;

static void modo_juego_raw(int activar) {
    if (activar) {
        if (tcgetattr(STDIN_FILENO, &termios_original) != 0) {
            termios_disponible = 0;
            return;
        }
        terminal_registrar(&termios_original);
        struct termios temporal = termios_original;
        /* Sin ISIG, Ctrl+C y Ctrl+Z llegan como bytes y el juego los trata
           como pausa; así el proceso no muere dejando el terminal en crudo. */
        temporal.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG);
        temporal.c_cc[VMIN] = 0;
        temporal.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &temporal) == 0) termios_disponible = 1;
        else terminal_restaurar();
    } else if (termios_disponible) {
        terminal_restaurar();
        termios_disponible = 0;
    }
}

static int tecla(int ms) {
    fd_set fds;
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    int r;
    do {
        r = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
    } while (r < 0 && errno == EINTR);
    if (r > 0) {
        char c;
        if (read(STDIN_FILENO, &c, 1) == 1) return (unsigned char)c;
    }
    return -1;
}

static void ruta_partida(const char *usuario, char *ruta, size_t n) {
    snprintf(ruta, n, DIR_DATOS "partida_%s.sav", usuario);
}

static int escribir_todo(int fd, const void *datos, size_t n) {
    const unsigned char *p = (const unsigned char *)datos;
    size_t escritos = 0;
    while (escritos < n) {
        ssize_t r = write(fd, p + escritos, n - escritos);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        escritos += (size_t)r;
    }
    return 0;
}

/* Escritura atómica: si MiniOS se interrumpe mientras guarda, el .sav anterior
   sigue intacto y el temporal no sustituye una partida válida. */
static int guardar_partida_archivo_sin_bloqueo(const Partida *p) {
    char ruta[96], temporal[112];
    ruta_partida(p->jugador, ruta, sizeof ruta);
    snprintf(temporal, sizeof temporal, "%s.tmp", ruta);

    int fd = open(temporal, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    (void)fchmod(fd, 0600);
    int ok = escribir_todo(fd, p, sizeof *p);
    if (ok == 0 && fsync(fd) != 0) ok = -1;
    if (close(fd) != 0) ok = -1;
    if (ok != 0 || rename(temporal, ruta) != 0) {
        unlink(temporal);
        return -1;
    }
    (void)chmod(ruta, 0600);
    return 0;
}

/* El autoguardado y el guardado manual comparten el mismo .tmp; el mutex
   serializa su escritura para que ningún rename borre el temporal del otro. */
static int guardar_partida_archivo(const Partida *p) {
    pthread_mutex_lock(&mutex_guardado);
    int resultado = guardar_partida_archivo_sin_bloqueo(p);
    pthread_mutex_unlock(&mutex_guardado);
    return resultado;
}

static void *hilo_autoguardado(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&mutex_control);
        if (!detener_autoguardado) {
            struct timespec limite;
            clock_gettime(CLOCK_REALTIME, &limite);
            limite.tv_sec += INTERVALO_AUTOGUARDADO;
            while (!detener_autoguardado) {
                int r = pthread_cond_timedwait(&cond_autoguardado, &mutex_control, &limite);
                if (r == ETIMEDOUT) break;
            }
        }
        int detener = detener_autoguardado;
        pthread_mutex_unlock(&mutex_control);
        if (detener) break;

        Partida copia;
        pthread_mutex_lock(&mutex_partida);
        copia = partida;
        copia.guardada_en = (long)time(NULL);
        pthread_mutex_unlock(&mutex_partida);

        if (copia.activo && guardar_partida_archivo(&copia) == 0)
            printf("\n[Autoguardado realizado: cada %d segundos]\n", INTERVALO_AUTOGUARDADO);
        else if (copia.activo)
            fprintf(stderr, "\n[AVISO] No se pudo autoguardar la partida.\n");
    }
    return NULL;
}

static int iniciar_hilo_autoguardado(pthread_t *hilo) {
    pthread_mutex_lock(&mutex_control);
    detener_autoguardado = 0;
    pthread_mutex_unlock(&mutex_control);
    int r = pthread_create(hilo, NULL, hilo_autoguardado, NULL);
    if (r != 0) {
        fprintf(stderr, "No se pudo iniciar el hilo de autoguardado: %s\n", strerror(r));
        return -1;
    }
    return 0;
}

static void detener_hilo_autoguardado(pthread_t hilo) {
    pthread_mutex_lock(&mutex_control);
    detener_autoguardado = 1;
    pthread_cond_signal(&cond_autoguardado);
    pthread_mutex_unlock(&mutex_control);
    pthread_join(hilo, NULL);
}

static void generar_comida(Partida *p) {
    int libre = 0;
    for (int intento = 0; intento < 1000 && !libre; ++intento) {
        p->comida.x = rand() % (ANCHO - 2) + 1;
        p->comida.y = rand() % (ALTO - 2) + 1;
        libre = 1;
        for (int i = 0; i < p->largo; ++i) {
            if (p->cuerpo[i].x == p->comida.x && p->cuerpo[i].y == p->comida.y) {
                libre = 0;
                break;
            }
        }
    }
}

static void renderizar(const Partida *p) {
    printf("\033[H\033[J");
    printf("=== MINI SNAKE | %s | Puntaje: %d | WASD mover | G guardar | Q salir ===\n",
           p->jugador, p->puntaje);
    for (int y = 0; y < ALTO; ++y) {
        for (int x = 0; x < ANCHO; ++x) {
            if (y == 0 || y == ALTO - 1 || x == 0 || x == ANCHO - 1) putchar('#');
            else if (x == p->cuerpo[0].x && y == p->cuerpo[0].y) putchar('O');
            else if (x == p->comida.x && y == p->comida.y) putchar('*');
            else {
                int es_cuerpo = 0;
                for (int i = 1; i < p->largo; ++i) {
                    if (p->cuerpo[i].x == x && p->cuerpo[i].y == y) {
                        putchar('o');
                        es_cuerpo = 1;
                        break;
                    }
                }
                if (!es_cuerpo) putchar(' ');
            }
        }
        putchar('\n');
    }
}

static void preparar_partida_nueva(const char *usuario) {
    memset(&partida, 0, sizeof partida);
    partida.magic = MAGIC_PARTIDA;
    partida.version = VERSION_PARTIDA;
    snprintf(partida.jugador, sizeof partida.jugador, "%s", usuario);
    partida.largo = 3;
    partida.direccion = -1; /* -1 espera el primer movimiento; 0 arriba, 1 derecha, 2 abajo, 3 izquierda */
    partida.cuerpo[0] = (Punto){ ANCHO / 2, ALTO / 2 };
    partida.cuerpo[1] = (Punto){ ANCHO / 2 - 1, ALTO / 2 };
    partida.cuerpo[2] = (Punto){ ANCHO / 2 - 2, ALTO / 2 };
    partida.activo = 1;
    generar_comida(&partida);
}

static int partida_valida(const Partida *p, const char *usuario) {
    return p->magic == MAGIC_PARTIDA && p->version == VERSION_PARTIDA &&
           strcmp(p->jugador, usuario) == 0 && p->largo >= 1 && p->largo <= MAX_COLA &&
           p->direccion >= -1 && p->direccion <= 3 && p->activo == 1 &&
           p->cuerpo[0].x > 0 && p->cuerpo[0].x < ANCHO - 1 &&
           p->cuerpo[0].y > 0 && p->cuerpo[0].y < ALTO - 1;
}

static void ejecutar_partida(void) {
    pthread_t hilo;
    int hilo_creado = iniciar_hilo_autoguardado(&hilo) == 0;
    int salir_solicitado = 0;

    modo_juego_raw(1);
    for (;;) {
        Partida vista;
        pthread_mutex_lock(&mutex_partida);
        vista = partida;
        pthread_mutex_unlock(&mutex_partida);

        if (!vista.activo) break;
        renderizar(&vista);
        int c = tecla(200);
        int guardar_manual = 0;
        int terminar = 0;

        pthread_mutex_lock(&mutex_partida);
        if (c == 'w' || c == 'W') { if (partida.direccion != 2) partida.direccion = 0; }
        if (c == 'd' || c == 'D') { if (partida.direccion != 3) partida.direccion = 1; }
        if (c == 's' || c == 'S') { if (partida.direccion != 0) partida.direccion = 2; }
        if (c == 'a' || c == 'A') { if (partida.direccion != 1) partida.direccion = 3; }
        if (c == 'g' || c == 'G') guardar_manual = 1;
        /* q, Ctrl+C o Ctrl+Z: pausar y guardar sin matar el proceso. */
        if (c == 'q' || c == 'Q' || c == 3 || c == 26) {
            salir_solicitado = 1;
            terminar = 1;
        }

        if (!terminar && partida.activo && partida.direccion >= 0) {
            Punto nueva = partida.cuerpo[0];
            if (partida.direccion == 0) --nueva.y;
            if (partida.direccion == 1) ++nueva.x;
            if (partida.direccion == 2) ++nueva.y;
            if (partida.direccion == 3) --nueva.x;

            if (nueva.x <= 0 || nueva.x >= ANCHO - 1 || nueva.y <= 0 || nueva.y >= ALTO - 1)
                partida.activo = 0;

            int comer = nueva.x == partida.comida.x && nueva.y == partida.comida.y;
            int limite_colision = comer ? partida.largo : partida.largo - 1;
            for (int i = 0; i < limite_colision; ++i) {
                if (partida.cuerpo[i].x == nueva.x && partida.cuerpo[i].y == nueva.y) {
                    partida.activo = 0;
                    break;
                }
            }

            if (partida.activo) {
                if (comer && partida.largo < MAX_COLA) ++partida.largo;
                for (int i = partida.largo - 1; i > 0; --i) partida.cuerpo[i] = partida.cuerpo[i - 1];
                partida.cuerpo[0] = nueva;
                if (comer) {
                    partida.puntaje += 10;
                    generar_comida(&partida);
                }
            }
        }
        Partida copia_manual;
        if (guardar_manual) {
            copia_manual = partida;
            copia_manual.guardada_en = (long)time(NULL);
        }
        pthread_mutex_unlock(&mutex_partida);

        if (guardar_manual) {
            if (guardar_partida_archivo(&copia_manual) == 0)
                printf("\nPartida guardada manualmente.\n");
            else fprintf(stderr, "\nNo se pudo guardar la partida.\n");
            struct timespec pausa = { .tv_sec = 0, .tv_nsec = 250000000L };
            nanosleep(&pausa, NULL);
        }
        if (terminar) break;
    }
    modo_juego_raw(0);

    pthread_mutex_lock(&mutex_partida);
    if (salir_solicitado) partida.activo = 1;
    Partida final = partida;
    final.guardada_en = (long)time(NULL);
    pthread_mutex_unlock(&mutex_partida);

    if (hilo_creado) detener_hilo_autoguardado(hilo);
    if (guardar_partida_archivo(&final) == 0) {
        if (salir_solicitado) printf("\nPartida pausada y guardada. Puede continuarla desde el menú.\n");
        else printf("\nÚltimo estado de la partida guardado.\n");
    } else {
        fprintf(stderr, "No se pudo guardar el estado final de la partida.\n");
    }

    if (!final.activo) {
        printf("\n=== GAME OVER ===\nPuntaje final: %d\n", final.puntaje);
        fs_guardar_record(final.jugador, final.puntaje);
    }
}

void juego_iniciar(void) {
    srand((unsigned int)time(NULL) ^ (unsigned int)getpid());
    pthread_mutex_lock(&mutex_partida);
    preparar_partida_nueva(seg_usuario_actual());
    pthread_mutex_unlock(&mutex_partida);
    ejecutar_partida();
}

void juego_continuar(void) {
    char ruta[96];
    ruta_partida(seg_usuario_actual(), ruta, sizeof ruta);

    int fd = open(ruta, O_RDONLY);
    if (fd < 0) {
        printf("No existe una partida guardada para %s.\n", seg_usuario_actual());
        return;
    }
    (void)fchmod(fd, 0600);
    Partida recuperada;
    ssize_t n = read(fd, &recuperada, sizeof recuperada);
    char extra;
    ssize_t sobrante = read(fd, &extra, 1);
    close(fd);

    if (n != (ssize_t)sizeof recuperada || sobrante != 0 ||
        !partida_valida(&recuperada, seg_usuario_actual())) {
        printf("La partida guardada no es válida o ya terminó. Inicie una partida nueva.\n");
        return;
    }

    pthread_mutex_lock(&mutex_partida);
    partida = recuperada; /* Se recupera el estado; NO se reinicializa el juego. */
    pthread_mutex_unlock(&mutex_partida);

    printf("Partida recuperada para %s. Puntaje: %d.\n", partida.jugador, partida.puntaje);
    sleep(1);
    ejecutar_partida();
}
