#include <signal.h>
#include <termios.h>
#include <unistd.h>
#include "terminal.h"

static struct termios configuracion_original;
static volatile sig_atomic_t hay_cambio = 0;

void terminal_registrar(const struct termios *config) {
    if (!config) return;
    configuracion_original = *config;
    hay_cambio = 1;
}

void terminal_restaurar(void) {
    if (!hay_cambio) return;
    hay_cambio = 0;
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &configuracion_original);
}
