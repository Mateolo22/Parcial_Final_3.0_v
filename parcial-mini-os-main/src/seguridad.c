#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>
#include "kernel.h"
#include "seguridad.h"
#include "bitacora.h"

#define ARCH_MATRIZ   DIR_DATOS "matriz.dat"
#define ARCH_USUARIOS DIR_DATOS "usuarios.dat"
#define MAX_INTENTOS  3
#define CLAVE_MINIMA  6

typedef struct {
    char nombre[32];
    unsigned long sal;
    unsigned long hash;
    int rol;
    int intentos_fallidos;
    int bloqueado;
} Usuario;

static Usuario sesion;
static int hay_sesion = 0;
static int denegaciones_sesion = 0;
static int alerta_denegaciones_emitida = 0;

/* Derechos por rol y objeto. El operador puede consultar procesos y el admin
   conserva siempre la capacidad de gestionar usuarios, bitácora y sistema. */
static unsigned char matriz[NUM_ROLES][NUM_OBJETOS] = {
    /* reg   jue   par   pro   mat   sis   usr   bit   vir */
    { 0x0F, 0x10, 0x0F, 0x1F, 0x06, 0x12, 0x07, 0x02, 0x10 },
    { 0x07, 0x10, 0x07, 0x06, 0x02, 0x02, 0x04, 0x00, 0x10 },
    { 0x02, 0x00, 0x00, 0x02, 0x00, 0x02, 0x04, 0x00, 0x00 }
};

static const char *NOMBRE_ROL[NUM_ROLES] = { "admin", "operador", "invitado" };
static const char *NOMBRE_OBJ[NUM_OBJETOS] = {
    "registros", "juegos", "partidas", "procesos", "matriz",
    "sistema", "usuarios", "bitacora", "virtualizacion"
};

static unsigned long hash_clave(const char *clave, unsigned long sal) {
    /* Hash didáctico con sal, acorde al alcance del taller. No sustituye
       Argon2/bcrypt/yescrypt en un sistema operativo de producción. */
    unsigned long h = 5381UL ^ sal;
    for (const unsigned char *p = (const unsigned char *)clave; *p; ++p)
        h = ((h << 5) + h) + *p;
    return h;
}

static unsigned long generar_sal(void) {
    unsigned long sal = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, &sal, sizeof sal);
        close(fd);
        if (n == (ssize_t)sizeof sal && sal != 0) return sal;
    }
    sal = ((unsigned long)time(NULL) << 16) ^ (unsigned long)getpid() ^ (unsigned long)rand();
    return sal ? sal : 0x9E3779B9UL;
}

static void leer_clave(const char *prompt, char *buf, size_t n) {
    struct termios original, temporal;
    int ocultar = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &original) == 0;
    if (ocultar) {
        temporal = original;
        temporal.c_lflag &= (tcflag_t)~ECHO;
        (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &temporal);
    }

    leer_linea(prompt, buf, (int)n);

    if (ocultar) {
        (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
        putchar('\n');
    }
}

static int escribir_completo(int fd, const void *datos, size_t n) {
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

static int nombre_valido(const char *nombre) {
    if (!nombre || !*nombre || strlen(nombre) >= sizeof sesion.nombre) return 0;
    for (const unsigned char *p = (const unsigned char *)nombre; *p; ++p) {
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' )) return 0;
    }
    return 1;
}

static int buscar_usuario(const char *nombre, Usuario *u, off_t *pos) {
    int fd = open(ARCH_USUARIOS, O_RDONLY);
    if (fd < 0) return 0;
    (void)fchmod(fd, 0600);
    off_t p = 0;
    Usuario actual;
    while (read(fd, &actual, sizeof actual) == (ssize_t)sizeof actual) {
        if (strcmp(actual.nombre, nombre) == 0) {
            if (u) *u = actual;
            if (pos) *pos = p;
            close(fd);
            return 1;
        }
        p += (off_t)sizeof actual;
    }
    close(fd);
    return 0;
}

static int crear_usuario(const char *nombre, const char *clave, Rol rol) {
    if (!nombre_valido(nombre) || !clave || strlen(clave) < CLAVE_MINIMA ||
        rol < 0 || rol >= NUM_ROLES) return -1;
    if (buscar_usuario(nombre, NULL, NULL)) return -2;

    Usuario u;
    memset(&u, 0, sizeof u);
    snprintf(u.nombre, sizeof u.nombre, "%s", nombre);
    u.sal = generar_sal();
    u.hash = hash_clave(clave, u.sal);
    u.rol = (int)rol;

    int fd = open(ARCH_USUARIOS, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return -1;
    (void)fchmod(fd, 0600);
    int ok = escribir_completo(fd, &u, sizeof u);
    if (ok == 0 && fsync(fd) != 0) ok = -1;
    close(fd);
    return ok;
}

static int inicializar_usuarios(void) {
    struct stat st;
    if (stat(ARCH_USUARIOS, &st) == 0 && st.st_size >= (off_t)sizeof(Usuario)) {
        (void)chmod(ARCH_USUARIOS, 0600);
        return 0;
    }

    srand((unsigned int)time(NULL) ^ (unsigned int)getpid());
    int fd = open(ARCH_USUARIOS, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    (void)fchmod(fd, 0600);
    close(fd);

    if (crear_usuario("admin", "admin123", ROL_ADMIN) != 0 ||
        crear_usuario("operador", "oper123", ROL_OPERADOR) != 0 ||
        crear_usuario("invitado", "inv123", ROL_INVITADO) != 0) return -1;

    printf("Usuarios iniciales creados. Cambie sus claves desde el menú.\n");
    return 0;
}

static int guardar_usuario(const Usuario *u, off_t pos) {
    int fd = open(ARCH_USUARIOS, O_WRONLY);
    if (fd < 0) return -1;
    (void)fchmod(fd, 0600);
    ssize_t n = pwrite(fd, u, sizeof *u, pos);
    int ok = (n == (ssize_t)sizeof *u && fsync(fd) == 0) ? 0 : -1;
    close(fd);
    return ok;
}

int seg_login(void) {
    if (inicializar_usuarios() != 0) {
        fprintf(stderr, "No se pudo inicializar %s: %s\n", ARCH_USUARIOS, strerror(errno));
        return -1;
    }

    char nombre[32] = {0};
    char clave[64] = {0};
    Usuario u;
    off_t pos = 0;

    leer_linea("Usuario: ", nombre, sizeof nombre);
    if (feof(stdin)) return -1;
    leer_clave("Contraseña: ", clave, sizeof clave);
    if (feof(stdin)) return -1;

    if (!buscar_usuario(nombre, &u, &pos)) {
        auditar("LOGIN", nombre, "LOGIN FALLIDO");
        printf("Usuario o contraseña incorrectos.\n");
        return 0;
    }

    if (u.bloqueado) {
        auditar("LOGIN", nombre, "BLOQUEO");
        printf("Usuario o contraseña incorrectos.\n");
        return 0;
    }

    if (hash_clave(clave, u.sal) != u.hash) {
        ++u.intentos_fallidos;
        if (u.intentos_fallidos >= MAX_INTENTOS) {
            u.intentos_fallidos = MAX_INTENTOS;
            u.bloqueado = 1;
        }
        (void)guardar_usuario(&u, pos);
        auditar("LOGIN", nombre, u.bloqueado ? "BLOQUEO" : "LOGIN FALLIDO");
        printf("Usuario o contraseña incorrectos.\n");
        return 0;
    }

    u.intentos_fallidos = 0;
    (void)guardar_usuario(&u, pos);
    sesion = u;
    hay_sesion = 1;
    denegaciones_sesion = 0;
    alerta_denegaciones_emitida = 0;
    auditar("LOGIN", nombre, "LOGIN EXITOSO");
    return 1;
}

void seg_cerrar_sesion(void) {
    if (hay_sesion) auditar("LOGOUT", sesion.nombre, "OK");
    hay_sesion = 0;
    denegaciones_sesion = 0;
    alerta_denegaciones_emitida = 0;
    memset(&sesion, 0, sizeof sesion);
}

const char *seg_usuario_actual(void) {
    return hay_sesion ? sesion.nombre : "-";
}

const char *seg_rol_actual(void) {
    return hay_sesion && sesion.rol >= 0 && sesion.rol < NUM_ROLES ? NOMBRE_ROL[sesion.rol] : "-";
}

int seg_es_admin(void) {
    return hay_sesion && sesion.rol == ROL_ADMIN;
}

int seg_verificar(Objeto obj, unsigned char derecho) {
    if (obj < 0 || obj >= NUM_OBJETOS || derecho == 0 || (derecho & (unsigned char)~0x1F) != 0) {
        auditar("DERECHO_INVALIDO", "matriz", "DENEGADO");
        printf("  ACCESO DENEGADO: solicitud inválida.\n");
        return 0;
    }

    char accion[32];
    snprintf(accion, sizeof accion, "DERECHO_0x%02X", derecho);
    if (hay_sesion && sesion.rol >= 0 && sesion.rol < NUM_ROLES &&
        (matriz[sesion.rol][obj] & derecho) == derecho) {
        auditar(accion, NOMBRE_OBJ[obj], "PERMITIDO");
        return 1;
    }

    ++denegaciones_sesion;
    auditar(accion, NOMBRE_OBJ[obj], "DENEGADO");
    printf("  ACCESO DENEGADO\n");
    if (denegaciones_sesion >= 5 && !alerta_denegaciones_emitida) {
        alerta_denegaciones_emitida = 1;
        printf("  [ALERTA SEGURIDAD] %s acumula 5 accesos denegados en esta sesión.\n",
               seg_usuario_actual());
        auditar("ALERTA_SEGURIDAD", "sistema", "5_DENEGACIONES_EN_SESION");
    }
    return 0;
}

void seg_gestionar_usuarios(void) {
    if (!seg_es_admin()) {
        printf("Acceso denegado: solo el administrador gestiona usuarios.\n");
        auditar("GESTION_USUARIOS", "usuarios", "DENEGADO");
        return;
    }

    printf("\n--- GESTIÓN DE USUARIOS ---\n");
    printf("1. Listar usuarios\n2. Crear usuario\n3. Desbloquear usuario\n0. Volver\n");
    int op = leer_entero("Opción: ");

    if (op == 1) {
        int fd = open(ARCH_USUARIOS, O_RDONLY);
        if (fd < 0) {
            perror("No se pudo abrir usuarios.dat");
            return;
        }
        (void)fchmod(fd, 0600);
        Usuario u;
        printf("\n%-18s %-12s %-10s %-12s\n", "USUARIO", "ROL", "FALLOS", "ESTADO");
        while (read(fd, &u, sizeof u) == (ssize_t)sizeof u) {
            if (u.rol < 0 || u.rol >= NUM_ROLES) continue;
            printf("%-18s %-12s %-10d %-12s\n", u.nombre, NOMBRE_ROL[u.rol],
                   u.intentos_fallidos, u.bloqueado ? "BLOQUEADO" : "ACTIVO");
        }
        close(fd);
    } else if (op == 2) {
        char nombre[32], clave[64];
        leer_linea("Nombre (letras, números, _ o -): ", nombre, sizeof nombre);
        if (!nombre_valido(nombre)) {
            printf("Nombre inválido. Use entre 1 y 31 caracteres permitidos.\n");
            return;
        }
        leer_clave("Contraseña (mínimo 6 caracteres): ", clave, sizeof clave);
        if (strlen(clave) < CLAVE_MINIMA) {
            printf("La contraseña debe tener al menos 6 caracteres.\n");
            return;
        }
        int r = leer_entero("Rol (0=admin, 1=operador, 2=invitado): ");
        if (r < 0 || r >= NUM_ROLES) {
            printf("Rol inválido.\n");
            return;
        }
        int resultado = crear_usuario(nombre, clave, (Rol)r);
        if (resultado == -2) printf("Ya existe un usuario con ese nombre.\n");
        else if (resultado != 0) printf("No se pudo crear el usuario.\n");
        else {
            auditar("CREAR_USUARIO", nombre, "OK");
            printf("Usuario creado correctamente.\n");
        }
    } else if (op == 3) {
        char nombre[32];
        Usuario u;
        off_t pos;
        leer_linea("Usuario a desbloquear: ", nombre, sizeof nombre);
        if (buscar_usuario(nombre, &u, &pos)) {
            u.bloqueado = 0;
            u.intentos_fallidos = 0;
            if (guardar_usuario(&u, pos) == 0) {
                auditar("DESBLOQUEAR_USUARIO", nombre, "OK");
                printf("Usuario desbloqueado.\n");
            } else printf("No se pudo guardar el cambio.\n");
        } else printf("Usuario no encontrado.\n");
    } else if (op != 0) {
        printf("Opción inválida.\n");
    }
}

void seg_cambiar_clave(void) {
    if (!hay_sesion) {
        printf("Debe iniciar sesión.\n");
        return;
    }

    char actual[64] = {0}, nueva[64] = {0};
    leer_clave("Contraseña actual: ", actual, sizeof actual);
    if (hash_clave(actual, sesion.sal) != sesion.hash) {
        auditar("CAMBIO_CLAVE", sesion.nombre, "CLAVE_ACTUAL_INCORRECTA");
        printf("Contraseña actual incorrecta.\n");
        return;
    }

    leer_clave("Nueva contraseña (mínimo 6 caracteres): ", nueva, sizeof nueva);
    if (strlen(nueva) < CLAVE_MINIMA) {
        printf("La nueva contraseña debe tener al menos 6 caracteres.\n");
        return;
    }

    Usuario u;
    off_t pos;
    if (!buscar_usuario(sesion.nombre, &u, &pos)) {
        printf("No se encontró el usuario de la sesión.\n");
        return;
    }
    u.sal = generar_sal();
    u.hash = hash_clave(nueva, u.sal);
    if (guardar_usuario(&u, pos) != 0) {
        printf("No se pudo guardar la nueva contraseña.\n");
        return;
    }
    sesion.sal = u.sal;
    sesion.hash = u.hash;
    auditar("CAMBIO_CLAVE", sesion.nombre, "OK");
    printf("Contraseña actualizada.\n");
}

void seg_guardar_matriz(void) {
    int fd = open(ARCH_MATRIZ, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        perror("No se pudo guardar matriz.dat");
        return;
    }
    (void)fchmod(fd, 0600);
    int ok = escribir_completo(fd, matriz, sizeof matriz);
    if (ok == 0) ok = fsync(fd);
    close(fd);
    if (ok != 0) perror("Error guardando la matriz");
}

void seg_cargar_matriz(void) {
    struct stat st;
    if (stat(ARCH_MATRIZ, &st) != 0 || st.st_size != (off_t)sizeof matriz) {
        printf("Matriz inexistente o de versión anterior: se genera la matriz v3.0.\n");
        seg_guardar_matriz();
        return;
    }

    int fd = open(ARCH_MATRIZ, O_RDONLY);
    if (fd < 0) {
        seg_guardar_matriz();
        return;
    }
    (void)fchmod(fd, 0600);
    ssize_t n = read(fd, matriz, sizeof matriz);
    close(fd);
    if (n != (ssize_t)sizeof matriz) {
        /* Los valores iniciales estáticos vuelven a ser la política segura. */
        const unsigned char predeterminada[NUM_ROLES][NUM_OBJETOS] = {
            { 0x0F, 0x10, 0x0F, 0x1F, 0x06, 0x12, 0x07, 0x02, 0x10 },
            { 0x07, 0x10, 0x07, 0x06, 0x02, 0x02, 0x04, 0x00, 0x10 },
            { 0x02, 0x00, 0x00, 0x02, 0x00, 0x02, 0x04, 0x00, 0x00 }
        };
        memcpy(matriz, predeterminada, sizeof matriz);
        seg_guardar_matriz();
    }

    /* Todos los roles pueden actualizar su propia clave. La gestión de otras
       cuentas sigue siendo exclusiva del admin (requiere D_CREATE y rol admin). */
    int corregida = 0;
    for (int r = 0; r < NUM_ROLES; ++r) {
        if (!(matriz[r][OBJ_USUARIOS] & D_UPDATE)) {
            matriz[r][OBJ_USUARIOS] |= D_UPDATE;
            corregida = 1;
        }
    }
    if (corregida) seg_guardar_matriz();
}

void seg_mostrar_matriz(void) {
    if (!hay_sesion) return;
    printf("\n%-10s %-8s %-8s %-8s %-8s %-8s %-8s %-8s %-8s %-8s\n",
           "ROL", "REGIST", "JUEGOS", "PARTID", "PROCES", "MATRIZ", "SISTEM", "USUARI", "BITACO", "VIRTUA");
    for (int r = 0; r < NUM_ROLES; ++r) {
        printf("%-10s ", NOMBRE_ROL[r]);
        for (int o = 0; o < NUM_OBJETOS; ++o) {
            char d[6] = "-----";
            unsigned char val = matriz[r][o];
            if (val & D_CREATE) d[0] = 'C';
            if (val & D_READ)   d[1] = 'R';
            if (val & D_UPDATE) d[2] = 'U';
            if (val & D_DELETE) d[3] = 'D';
            if (val & D_EXEC)   d[4] = 'X';
            printf("%-8s ", d);
        }
        putchar('\n');
    }
}

void seg_modificar_matriz(void) {
    if (!seg_es_admin()) {
        printf("Solo el administrador puede modificar la matriz.\n");
        return;
    }
    int r = leer_entero("Rol (0=admin, 1=operador, 2=invitado): ");
    if (r < 0 || r >= NUM_ROLES) return;
    int o = leer_entero("Objeto (0=registros ... 8=virtualización): ");
    if (o < 0 || o >= NUM_OBJETOS) return;
    int n = leer_entero("Nuevos derechos (0..31): ");
    if (n < 0 || n > 31) return;

    if (r == ROL_ADMIN &&
        ((o == OBJ_MATRIZ && !(n & D_UPDATE)) ||
         (o == OBJ_USUARIOS && !(n & D_CREATE)) ||
         (o == OBJ_BITACORA && !(n & D_READ)) ||
         (o == OBJ_SISTEMA && !(n & D_EXEC)))) {
        printf("Cambio rechazado: el admin no puede quitarse permisos críticos.\n");
        return;
    }

    matriz[r][o] = (unsigned char)n;
    seg_guardar_matriz();
    auditar("MODIFICAR_MATRIZ", NOMBRE_OBJ[o], "OK");
    printf("Matriz actualizada y guardada.\n");
}
