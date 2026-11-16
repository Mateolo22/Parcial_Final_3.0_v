#ifndef TERMINAL_H
#define TERMINAL_H

#include <termios.h>

/* Guarda la configuración del terminal previa a desactivar el eco o entrar
   en modo de juego, para poder restaurarla desde un manejador de señales. */
void terminal_registrar(const struct termios *config);
void terminal_restaurar(void);

#endif
