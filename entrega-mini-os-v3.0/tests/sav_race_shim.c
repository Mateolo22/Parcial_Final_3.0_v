/* Shim de prueba: retrasa 500 ms el open() del .sav.tmp para forzar la
   carrera entre el hilo de autoguardado y el guardado manual.
   Se compila como biblioteca compartida y se carga con LD_PRELOAD. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static long tid(void) { return (long)syscall(SYS_gettid); }

static double ahora(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

typedef int (*open_fn)(const char *, int, ...);

int open(const char *ruta, int flags, ...) {
    static open_fn real = NULL;
    if (!real) real = (open_fn)dlsym(RTLD_NEXT, "open");

    mode_t modo = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        modo = (mode_t)va_arg(ap, int);
        va_end(ap);
    }

    int fd = real(ruta, flags, modo);
    if (fd >= 0 && ruta && strstr(ruta, ".sav.tmp")) {
        fprintf(stderr, "[SHIM %.3f tid=%ld] open(%s) fd=%d -> duerme 500ms\n",
                ahora(), tid(), ruta, fd);
        usleep(500000);
    }
    return fd;
}

typedef int (*rename_fn)(const char *, const char *);

int rename(const char *viejo, const char *nuevo) {
    static rename_fn real = NULL;
    if (!real) real = (rename_fn)dlsym(RTLD_NEXT, "rename");

    int r = real(viejo, nuevo);
    int e = errno;
    if (viejo && strstr(viejo, ".sav.tmp")) {
        if (r == 0) {
            fprintf(stderr, "[SHIM %.3f tid=%ld] rename(%s -> %s) = 0 (ok)\n",
                    ahora(), tid(), viejo, nuevo);
        } else {
            fprintf(stderr, "[SHIM %.3f tid=%ld] rename(%s -> %s) = -1 errno=%d (%s)\n",
                    ahora(), tid(), viejo, nuevo, e, strerror(e));
        }
    }
    return r;
}
