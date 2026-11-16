#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "kernel.h"
#include "seguridad.h"
#include "bitacora.h"
#include "fs.h"

#define MAGIC 0x4D494E49U
#define VERSION_DISCO 3U
#define MAX_REG 100U

typedef struct {
    unsigned int magic;
    unsigned int version;
    unsigned int total;
    unsigned int usados;
    unsigned int siguiente_id;
    unsigned int limpio;
} Superbloque;

typedef struct {
    int id;
    int activo;
    char nombre[50];
    char descripcion[100];
    char propietario[32];
} Registro;

static int fd_disco = -1;
static int fd_espejo = -1;
static Superbloque sb;

static off_t pos_registro(unsigned int i) {
    return (off_t)sizeof(Superbloque) + (off_t)i * (off_t)sizeof(Registro);
}

static int pwrite_completo(int fd, const void *datos, size_t n, off_t pos) {
    const unsigned char *p = (const unsigned char *)datos;
    size_t escritos = 0;
    while (escritos < n) {
        ssize_t r = pwrite(fd, p + escritos, n - escritos, pos + (off_t)escritos);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        escritos += (size_t)r;
    }
    return 0;
}

static int pread_completo(int fd, void *datos, size_t n, off_t pos) {
    unsigned char *p = (unsigned char *)datos;
    size_t leidos = 0;
    while (leidos < n) {
        ssize_t r = pread(fd, p + leidos, n - leidos, pos + (off_t)leidos);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        leidos += (size_t)r;
    }
    return 0;
}

static int copiar_disco(int origen, int destino) {
    char bloque[4096];
    if (ftruncate(destino, 0) != 0) return -1;
    if (lseek(origen, 0, SEEK_SET) < 0 || lseek(destino, 0, SEEK_SET) < 0) return -1;

    for (;;) {
        ssize_t n = read(origen, bloque, sizeof bloque);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -1;
        if (n == 0) break;
        size_t escritos = 0;
        while (escritos < (size_t)n) {
            ssize_t w = write(destino, bloque + escritos, (size_t)n - escritos);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) return -1;
            escritos += (size_t)w;
        }
    }
    return fsync(destino);
}

/* Escribe el mismo bloque en ambos discos. Si una copia falla pero la otra
   quedó bien, intenta reconstruir inmediatamente la copia defectuosa. */
static int escribir_en_ambos(off_t pos, const void *datos, size_t n) {
    int ok_principal = pwrite_completo(fd_disco, datos, n, pos) == 0;
    int ok_espejo = pwrite_completo(fd_espejo, datos, n, pos) == 0;

    if (ok_principal && ok_espejo) return 0;
    if (ok_principal && !ok_espejo) {
        fprintf(stderr, "[RAID 1] Falló el espejo; intentando reconstruirlo.\n");
        if (copiar_disco(fd_disco, fd_espejo) == 0) return 0;
    } else if (!ok_principal && ok_espejo) {
        fprintf(stderr, "[RAID 1] Falló el principal; intentando reconstruirlo.\n");
        if (copiar_disco(fd_espejo, fd_disco) == 0) return 0;
    }
    return -1;
}

static int guardar_superbloque(void) {
    if (escribir_en_ambos(0, &sb, sizeof sb) != 0) return -1;
    if (fsync(fd_disco) != 0 || fsync(fd_espejo) != 0) return -1;
    return 0;
}

static int superbloque_valido(const Superbloque *s) {
    return s->magic == MAGIC && s->version == VERSION_DISCO && s->total == MAX_REG &&
           s->siguiente_id > 0 && s->usados <= s->total;
}

static void formatear_arreglo(void) {
    memset(&sb, 0, sizeof sb);
    sb.magic = MAGIC;
    sb.version = VERSION_DISCO;
    sb.total = MAX_REG;
    sb.siguiente_id = 1;
    sb.limpio = 1;

    (void)ftruncate(fd_disco, (off_t)sizeof(Superbloque) + (off_t)MAX_REG * (off_t)sizeof(Registro));
    (void)ftruncate(fd_espejo, (off_t)sizeof(Superbloque) + (off_t)MAX_REG * (off_t)sizeof(Registro));
    Registro vacio;
    memset(&vacio, 0, sizeof vacio);
    for (unsigned int i = 0; i < MAX_REG; ++i)
        (void)escribir_en_ambos(pos_registro(i), &vacio, sizeof vacio);
    (void)guardar_superbloque();
}

int fs_montar(const char *ruta_principal, const char *ruta_espejo) {
    fd_disco = open(ruta_principal, O_RDWR | O_CREAT, 0600);
    if (fd_disco < 0) {
        perror("No se pudo abrir minios.disk");
        return -1;
    }
    fd_espejo = open(ruta_espejo, O_RDWR | O_CREAT, 0600);
    if (fd_espejo < 0) {
        perror("No se pudo abrir minios_espejo.disk");
        close(fd_disco);
        fd_disco = -1;
        return -1;
    }
    (void)fchmod(fd_disco, 0600);
    (void)fchmod(fd_espejo, 0600);

    Superbloque sb1 = {0}, sb2 = {0};
    int ok1 = pread_completo(fd_disco, &sb1, sizeof sb1, 0) == 0 && superbloque_valido(&sb1);
    int ok2 = pread_completo(fd_espejo, &sb2, sizeof sb2, 0) == 0 && superbloque_valido(&sb2);

    if (!ok1 && ok2) {
        printf("[RAID 1] Disco principal ausente o dañado. Reconstruyendo desde el espejo...\n");
        if (copiar_disco(fd_espejo, fd_disco) != 0) return -1;
        sb = sb2;
        printf("[RAID 1] Disco principal reconstruido.\n");
    } else if (ok1 && !ok2) {
        printf("[RAID 1] Disco espejo ausente o dañado. Reconstruyendo desde el principal...\n");
        if (copiar_disco(fd_disco, fd_espejo) != 0) return -1;
        sb = sb1;
        printf("[RAID 1] Disco espejo reconstruido.\n");
    } else if (!ok1 && !ok2) {
        printf("[RAID 1] Ambos discos no son válidos. Se crea un arreglo nuevo.\n");
        formatear_arreglo();
    } else {
        sb = sb1;
        if (memcmp(&sb1, &sb2, sizeof sb1) != 0) {
            printf("[RAID 1] Los superbloques no coinciden; se sincroniza el espejo desde el principal.\n");
            if (copiar_disco(fd_disco, fd_espejo) != 0) return -1;
        }
    }

    if (!sb.limpio) printf("ADVERTENCIA: el disco no se desmontó correctamente (apagado sucio).\n");
    sb.limpio = 0;
    if (guardar_superbloque() != 0) {
        fprintf(stderr, "No se pudo actualizar el superbloque en ambos discos.\n");
        return -1;
    }
    return 0;
}

void fs_desmontar(void) {
    if (fd_disco < 0 && fd_espejo < 0) return;
    sb.limpio = 1;
    (void)guardar_superbloque();
    if (fd_disco >= 0) {
        close(fd_disco);
        fd_disco = -1;
    }
    if (fd_espejo >= 0) {
        close(fd_espejo);
        fd_espejo = -1;
    }
}

static int leer_registro(unsigned int i, Registro *r) {
    if (fd_disco < 0 || i >= MAX_REG) return -1;
    return pread_completo(fd_disco, r, sizeof *r, pos_registro(i));
}

static int escribir_registro(unsigned int i, const Registro *r) {
    if (i >= MAX_REG || escribir_en_ambos(pos_registro(i), r, sizeof *r) != 0) return -1;
    if (fsync(fd_disco) != 0 || fsync(fd_espejo) != 0) return -1;
    return 0;
}

void fs_crear(void) {
    Registro nuevo;
    memset(&nuevo, 0, sizeof nuevo);
    int slot = -1;
    for (unsigned int i = 0; i < MAX_REG; ++i) {
        Registro tmp;
        if (leer_registro(i, &tmp) == 0 && !tmp.activo) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        printf("Disco lleno.\n");
        return;
    }

    leer_linea("Nombre: ", nuevo.nombre, sizeof nuevo.nombre);
    if (feof(stdin) || nuevo.nombre[0] == '\0') {
        printf("Nombre vacío; operación cancelada.\n");
        return;
    }
    leer_linea("Descripción: ", nuevo.descripcion, sizeof nuevo.descripcion);
    nuevo.id = (int)sb.siguiente_id;
    nuevo.activo = 1;
    snprintf(nuevo.propietario, sizeof nuevo.propietario, "%s", seg_usuario_actual());

    if (escribir_registro((unsigned int)slot, &nuevo) != 0) {
        printf("No se pudo escribir el registro en ambos discos.\n");
        return;
    }
    ++sb.siguiente_id;
    ++sb.usados;
    if (guardar_superbloque() != 0) printf("Aviso: registro escrito, pero falló la actualización del superbloque.\n");
    else printf("Registro ID %d creado en la ranura %d (principal + espejo).\n", nuevo.id, slot);
}

void fs_guardar_record(const char *jugador, int puntaje) {
    int slot = -1;
    for (unsigned int i = 0; i < MAX_REG; ++i) {
        Registro tmp;
        if (leer_registro(i, &tmp) == 0 && !tmp.activo) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        printf("No hay espacio para guardar el récord en el disco virtual.\n");
        return;
    }

    Registro r;
    memset(&r, 0, sizeof r);
    r.id = (int)sb.siguiente_id;
    r.activo = 1;
    snprintf(r.nombre, sizeof r.nombre, "Record Snake - %.24s", jugador);
    snprintf(r.descripcion, sizeof r.descripcion, "Puntaje alcanzado: %d", puntaje);
    snprintf(r.propietario, sizeof r.propietario, "%s", jugador);

    if (escribir_registro((unsigned int)slot, &r) != 0) {
        printf("No se pudo guardar el récord en el arreglo RAID 1.\n");
        return;
    }
    ++sb.siguiente_id;
    ++sb.usados;
    (void)guardar_superbloque();
    printf("Récord guardado en el disco virtual y en su espejo (ID %d).\n", r.id);
}

void fs_listar(void) {
    printf("\n%-5s %-25s %-35s %-15s\n", "ID", "NOMBRE", "DESCRIPCIÓN", "PROPIETARIO");
    printf("-----------------------------------------------------------------------------------\n");
    for (unsigned int i = 0; i < MAX_REG; ++i) {
        Registro r;
        if (leer_registro(i, &r) == 0 && r.activo)
            printf("%-5d %-25.25s %-35.35s %-15.15s\n", r.id, r.nombre, r.descripcion, r.propietario);
    }
}

void fs_actualizar(void) {
    int id = leer_entero("ID a actualizar: ");
    if (id <= 0) return;
    for (unsigned int i = 0; i < MAX_REG; ++i) {
        Registro r;
        if (leer_registro(i, &r) == 0 && r.activo && r.id == id) {
            if (!seg_es_admin() && strcmp(r.propietario, seg_usuario_actual()) != 0) {
                printf("ACCESO DENEGADO: no es propietario del registro.\n");
                auditar("ACTUALIZAR_REGISTRO", "propiedad", "DENEGADO");
                return;
            }
            leer_linea("Nuevo nombre: ", r.nombre, sizeof r.nombre);
            leer_linea("Nueva descripción: ", r.descripcion, sizeof r.descripcion);
            if (escribir_registro(i, &r) != 0) printf("No se pudo actualizar en ambos discos.\n");
            else printf("Registro %d actualizado en ambos discos.\n", id);
            return;
        }
    }
    printf("Registro no encontrado.\n");
}

void fs_borrar(void) {
    int id = leer_entero("ID a borrar: ");
    if (id <= 0) return;
    for (unsigned int i = 0; i < MAX_REG; ++i) {
        Registro r;
        if (leer_registro(i, &r) == 0 && r.activo && r.id == id) {
            if (!seg_es_admin() && strcmp(r.propietario, seg_usuario_actual()) != 0) {
                printf("ACCESO DENEGADO: no es propietario del registro.\n");
                auditar("ELIMINAR_REGISTRO", "propiedad", "DENEGADO");
                return;
            }
            r.activo = 0;
            if (escribir_registro(i, &r) != 0) {
                printf("No se pudo eliminar en ambos discos.\n");
                return;
            }
            if (sb.usados > 0) --sb.usados;
            (void)guardar_superbloque();
            printf("Registro %d eliminado. Su ranura queda libre para reutilizarse.\n", id);
            return;
        }
    }
    printf("Registro no encontrado.\n");
}

void fs_estado(void) {
    printf("\n=== ESTADO DEL DISCO VIRTUAL RAID 1 ===\n");
    printf("Número mágico:     0x%X\n", sb.magic);
    printf("Versión:           %u\n", sb.version);
    printf("Estado:            %s\n", sb.limpio ? "Limpio" : "En uso (sucio)");
    printf("Capacidad total:    %u ranuras\n", sb.total);
    printf("Ranuras usadas:     %u\n", sb.usados);
    printf("Ranuras libres:     %u\n", sb.total - sb.usados);
    printf("Siguiente ID:       %u\n", sb.siguiente_id);
    printf("\nMapa ([X]=usado, [.]=libre):\n");
    for (unsigned int i = 0; i < sb.total; ++i) {
        Registro r;
        if (leer_registro(i, &r) == 0 && r.activo) printf("[X]");
        else printf("[.]");
        if ((i + 1) % 20 == 0) putchar('\n');
    }
    printf("=======================================\n");
}
