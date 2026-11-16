#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include "kernel.h"
#include "seguridad.h"
#include "bitacora.h"

#define ARCH_BITACORA DIR_DATOS "bitacora.log"
#define MAX_USUARIOS_RESUMEN 256

typedef struct {
    char usuario[32];
    int denegaciones;
} ConteoUsuario;

static void quitar_espacios(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
        s[--n] = '\0';
    size_t inicio = 0;
    while (s[inicio] == ' ' || s[inicio] == '\t') ++inicio;
    if (inicio) memmove(s, s + inicio, strlen(s + inicio) + 1);
}

static int escribir_completo(int fd, const char *buf, size_t n) {
    size_t total = 0;
    while (total < n) {
        ssize_t r = write(fd, buf + total, n - total);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        total += (size_t)r;
    }
    return 0;
}

void auditar(const char *accion, const char *objeto, const char *resultado) {
    char fecha[32];
    char linea[320];
    time_t ahora = time(NULL);
    struct tm tm_local;

    if (!localtime_r(&ahora, &tm_local)) return;
    if (strftime(fecha, sizeof fecha, "%Y-%m-%d %H:%M:%S", &tm_local) == 0) return;

    int n = snprintf(linea, sizeof linea, "%s | %-12.12s | %-10.10s | %-20.20s | %-18.18s | %s\n",
                     fecha, seg_usuario_actual(), seg_rol_actual(),
                     accion ? accion : "-", objeto ? objeto : "-",
                     resultado ? resultado : "-");
    if (n < 0 || (size_t)n >= sizeof linea) return;

    int fd = open(ARCH_BITACORA, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return;
    (void)fchmod(fd, 0600);
    if (escribir_completo(fd, linea, (size_t)n) == 0) (void)fsync(fd);
    close(fd);
}

void bitacora_mostrar(void) {
    if (!seg_es_admin()) {
        printf("Acceso denegado: solo el administrador puede consultar la bitácora.\n");
        auditar("VER_BITACORA", "bitacora", "DENEGADO");
        return;
    }

    FILE *f = fopen(ARCH_BITACORA, "r");
    if (!f) {
        printf("Bitácora vacía o todavía no creada.\n");
        return;
    }

    (void)chmod(ARCH_BITACORA, 0600);
    char linea[384];
    int denegados = 0;
    int logins_fallidos = 0;
    ConteoUsuario usuarios[MAX_USUARIOS_RESUMEN];
    int cantidad_usuarios = 0;
    memset(usuarios, 0, sizeof usuarios);

    printf("\n================ BITÁCORA DE AUDITORÍA ================\n");
    printf("FECHA Y HORA       | USUARIO      | ROL        | ACCIÓN               | OBJETO             | RESULTADO\n");
    printf("------------------------------------------------------------------------------------------------\n");

    while (fgets(linea, sizeof linea, f)) {
        fputs(linea, stdout);

        char campos[6][96] = {{0}};
        int n = sscanf(linea, "%95[^|]|%95[^|]|%95[^|]|%95[^|]|%95[^|]|%95[^\n]",
                       campos[0], campos[1], campos[2], campos[3], campos[4], campos[5]);
        if (n < 6) continue;
        for (int i = 0; i < 6; ++i) quitar_espacios(campos[i]);

        if (strcmp(campos[5], "DENEGADO") == 0) {
            ++denegados;
            const char *usuario = campos[1];
            if (usuario[0] != '\0' && strcmp(usuario, "-") != 0) {
                int encontrado = 0;
                for (int i = 0; i < cantidad_usuarios; ++i) {
                    if (strcmp(usuarios[i].usuario, usuario) == 0) {
                        ++usuarios[i].denegaciones;
                        encontrado = 1;
                        break;
                    }
                }
                if (!encontrado && cantidad_usuarios < MAX_USUARIOS_RESUMEN) {
                    snprintf(usuarios[cantidad_usuarios].usuario,
                             sizeof usuarios[cantidad_usuarios].usuario, "%.31s", usuario);
                    usuarios[cantidad_usuarios].denegaciones = 1;
                    ++cantidad_usuarios;
                }
            }
        }
        if (strcmp(campos[5], "LOGIN FALLIDO") == 0 || strcmp(campos[5], "BLOQUEO") == 0)
            ++logins_fallidos;
    }
    fclose(f);

    const char *usuario_mas_denegaciones = "Ninguno";
    int max_denegaciones = 0;
    for (int i = 0; i < cantidad_usuarios; ++i) {
        if (usuarios[i].denegaciones > max_denegaciones) {
            max_denegaciones = usuarios[i].denegaciones;
            usuario_mas_denegaciones = usuarios[i].usuario;
        }
    }

    printf("------------------------------------------------------------------------------------------------\n");
    printf("RESUMEN DE AUDITORÍA\n");
    printf("Accesos denegados:                 %d\n", denegados);
    printf("Intentos de login fallidos:        %d\n", logins_fallidos);
    printf("Usuario con más denegaciones:      %s (%d)\n",
           usuario_mas_denegaciones, max_denegaciones);
    printf("========================================================\n");
}
