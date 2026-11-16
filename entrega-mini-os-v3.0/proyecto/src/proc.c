#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include "kernel.h"
#include "seguridad.h"
#include "proc.h"
#include "bitacora.h"

#define MAX_PROC 20

typedef enum { EJECUTANDO, SUSPENDIDO, TERMINADO } Estado;

typedef struct {
    pid_t pid;
    char nombre[32];
    char usuario[32];
    Estado estado;
    time_t inicio;
} PCB;

static PCB tabla_proc[MAX_PROC];
static int total_proc = 0;

static const char *estado_str(Estado estado) {
    if (estado == EJECUTANDO) return "EJECUTANDO";
    if (estado == SUSPENDIDO) return "SUSPENDIDO";
    return "TERMINADO";
}

static int esperar_hijo(PCB *proceso) {
    int estado = 0;
    pid_t r;
    do {
        r = waitpid(proceso->pid, &estado, WUNTRACED);
    } while (r < 0 && errno == EINTR);

    if (r < 0) {
        if (errno == ECHILD) proceso->estado = TERMINADO;
        else perror("waitpid");
        return -1;
    }
    proceso->estado = WIFSTOPPED(estado) ? SUSPENDIDO : TERMINADO;
    return 0;
}

static int es_propietario(const PCB *p) {
    return seg_es_admin() || strcmp(p->usuario, seg_usuario_actual()) == 0;
}

static void mostrar_memoria(pid_t pid) {
    char ruta[64], linea[160];
    unsigned long vm_kb = 0, rss_kb = 0;
    snprintf(ruta, sizeof ruta, "/proc/%ld/status", (long)pid);
    FILE *f = fopen(ruta, "r");
    if (!f) {
        printf("    Memoria: proceso no disponible (quizá ya terminó).\n");
        return;
    }

    while (fgets(linea, sizeof linea, f)) {
        if (sscanf(linea, "VmSize: %lu kB", &vm_kb) == 1) continue;
        if (sscanf(linea, "VmRSS: %lu kB", &rss_kb) == 1) continue;
    }
    fclose(f);

    long pagina = sysconf(_SC_PAGESIZE);
    if (pagina <= 0) pagina = 4096;
    unsigned long bytes_vm = vm_kb * 1024UL;
    unsigned long bytes_rss = rss_kb * 1024UL;
    unsigned long paginas_vm = (bytes_vm + (unsigned long)pagina - 1UL) / (unsigned long)pagina;
    unsigned long paginas_rss = (bytes_rss + (unsigned long)pagina - 1UL) / (unsigned long)pagina;

    printf("    VmSize: %lu kB (%lu páginas) | VmRSS: %lu kB (%lu páginas) | Página: %ld bytes\n",
           vm_kb, paginas_vm, rss_kb, paginas_rss, pagina);
}

static void cambiar_manejadores_senales(struct sigaction *old_tstp, struct sigaction *old_int,
                                        int instalar) {
    if (instalar) {
        struct sigaction ignorar;
        memset(&ignorar, 0, sizeof ignorar);
        ignorar.sa_handler = SIG_IGN;
        sigemptyset(&ignorar.sa_mask);
        sigaction(SIGTSTP, &ignorar, old_tstp);
        sigaction(SIGINT, &ignorar, old_int);
    } else {
        sigaction(SIGTSTP, old_tstp, NULL);
        sigaction(SIGINT, old_int, NULL);
    }
}

static void proc_lanzar(const char *programa) {
    if (total_proc >= MAX_PROC) {
        printf("Tabla de procesos llena.\n");
        return;
    }

    struct sigaction viejo_tstp, viejo_int;
    cambiar_manejadores_senales(&viejo_tstp, &viejo_int, 1);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        cambiar_manejadores_senales(&viejo_tstp, &viejo_int, 0);
        return;
    }
    if (pid == 0) {
        struct sigaction defecto;
        memset(&defecto, 0, sizeof defecto);
        defecto.sa_handler = SIG_DFL;
        sigemptyset(&defecto.sa_mask);
        sigaction(SIGTSTP, &defecto, NULL);
        sigaction(SIGINT, &defecto, NULL);
        execlp(programa, programa, (char *)NULL);
        perror("No se pudo ejecutar el programa");
        _exit(127);
    }

    PCB *p = &tabla_proc[total_proc++];
    memset(p, 0, sizeof *p);
    p->pid = pid;
    snprintf(p->nombre, sizeof p->nombre, "%s", programa);
    snprintf(p->usuario, sizeof p->usuario, "%s", seg_usuario_actual());
    p->estado = EJECUTANDO;
    p->inicio = time(NULL);

    printf("Proceso %s iniciado con PID %ld. Use Ctrl+Z para suspenderlo.\n",
           programa, (long)pid);
    (void)esperar_hijo(p);
    cambiar_manejadores_senales(&viejo_tstp, &viejo_int, 0);

    if (p->estado == SUSPENDIDO)
        printf("Proceso %ld suspendido. Puede reanudarlo desde el menú.\n", (long)pid);
    else
        printf("Proceso %ld finalizó (estado registrado y proceso recolectado).\n", (long)pid);
}

void proc_listar(void) {
    printf("\n%-7s %-17s %-13s %-13s %-20s\n", "PID", "PROGRAMA", "DUEÑO", "ESTADO", "INICIO");
    printf("-------------------------------------------------------------------------------\n");
    for (int i = 0; i < total_proc; ++i) {
        char hora[32] = "-";
        struct tm tm_inicio;
        if (localtime_r(&tabla_proc[i].inicio, &tm_inicio))
            strftime(hora, sizeof hora, "%H:%M:%S %Y-%m-%d", &tm_inicio);
        printf("%-7ld %-17s %-13s %-13s %-20s\n", (long)tabla_proc[i].pid,
               tabla_proc[i].nombre, tabla_proc[i].usuario,
               estado_str(tabla_proc[i].estado), hora);
    }
    proc_monitor_memoria();
}

void proc_monitor_memoria(void) {
    long pagina = sysconf(_SC_PAGESIZE);
    printf("\n--- MONITOR DE MEMORIA DE PROCESOS ---\n");
    printf("Tamaño de página del sistema: %ld bytes\n", pagina > 0 ? pagina : 4096L);
    if (total_proc == 0) {
        printf("Todavía no hay procesos registrados.\n");
        return;
    }
    for (int i = 0; i < total_proc; ++i) {
        printf("PID %ld (%s, %s):\n", (long)tabla_proc[i].pid,
               tabla_proc[i].nombre, estado_str(tabla_proc[i].estado));
        if (tabla_proc[i].estado != TERMINADO) mostrar_memoria(tabla_proc[i].pid);
        else printf("    Proceso terminado; /proc ya no conserva sus métricas.\n");
    }
}

void proc_reanudar_id(pid_t pid) {
    for (int i = 0; i < total_proc; ++i) {
        PCB *p = &tabla_proc[i];
        if (p->pid == pid && p->estado == SUSPENDIDO) {
            if (!es_propietario(p)) {
                printf("ACCESO DENEGADO: no es propietario del proceso.\n");
                auditar("REANUDAR_PROCESO", "propiedad", "DENEGADO");
                return;
            }
            struct sigaction viejo_tstp, viejo_int;
            cambiar_manejadores_senales(&viejo_tstp, &viejo_int, 1);
            if (kill(pid, SIGCONT) != 0) {
                perror("SIGCONT");
                cambiar_manejadores_senales(&viejo_tstp, &viejo_int, 0);
                return;
            }
            p->estado = EJECUTANDO;
            (void)esperar_hijo(p);
            cambiar_manejadores_senales(&viejo_tstp, &viejo_int, 0);
            printf("Proceso %ld: %s.\n", (long)pid, estado_str(p->estado));
            return;
        }
    }
    printf("Proceso suspendido no encontrado.\n");
}

void proc_terminar_id(pid_t pid) {
    for (int i = 0; i < total_proc; ++i) {
        PCB *p = &tabla_proc[i];
        if (p->pid == pid && p->estado != TERMINADO) {
            if (!es_propietario(p)) {
                printf("ACCESO DENEGADO: no es propietario del proceso.\n");
                auditar("TERMINAR_PROCESO", "propiedad", "DENEGADO");
                return;
            }
            if (kill(pid, SIGTERM) != 0 && errno != ESRCH) {
                perror("SIGTERM");
                return;
            }
            (void)kill(pid, SIGCONT); /* permite terminar también un proceso detenido */
            int estado;
            pid_t r;
            do {
                r = waitpid(pid, &estado, 0);
            } while (r < 0 && errno == EINTR);
            if (r < 0 && errno != ECHILD) perror("waitpid");
            p->estado = TERMINADO;
            printf("Proceso %ld terminado y recolectado; no queda zombi.\n", (long)pid);
            return;
        }
    }
    printf("Proceso no encontrado o ya terminado.\n");
}

void proc_lanzar_menu(void) {
    printf("\n--- GESTIÓN DE PROCESOS ---\n");
    printf("1. Lanzar Tetris (BSDGames)\n");
    printf("2. Listar procesos y memoria\n");
    printf("3. Reanudar proceso suspendido\n");
    printf("4. Terminar proceso\n");
    printf("0. Volver\n");
    int op = leer_entero("Opción: ");
    if (op == 1) {
        if (seg_verificar(OBJ_PROCESOS, D_EXEC)) proc_lanzar("tetris-bsd");
    } else if (op == 2) {
        if (seg_verificar(OBJ_PROCESOS, D_READ)) proc_listar();
    } else if (op == 3) {
        if (!seg_verificar(OBJ_PROCESOS, D_UPDATE)) return;
        int pid = leer_entero("PID a reanudar: ");
        if (pid > 0) proc_reanudar_id((pid_t)pid);
    } else if (op == 4) {
        if (!seg_verificar(OBJ_PROCESOS, D_UPDATE)) return;
        int pid = leer_entero("PID a terminar: ");
        if (pid > 0) proc_terminar_id((pid_t)pid);
    } else if (op != 0) {
        printf("Opción inválida.\n");
    }
}
