#include <stdio.h>
#include <sys/stat.h>
#include "kernel.h"
#include "fs.h"
#include "seguridad.h"
#include "bitacora.h"
#include "virtualizacion.h"
#include "juego.h"
#include "proc.h"
#include "sistema.h"

static void mostrar_menu(void) {
    printf("\n========== MiniOS v%s | %s (%s) ==========\n",
           VERSION_MINIOS, seg_usuario_actual(), seg_rol_actual());
    printf(" 1. Crear registro                  (disco virtual)\n");
    printf(" 2. Consultar registros              (disco virtual)\n");
    printf(" 3. Actualizar registro               (disco virtual)\n");
    printf(" 4. Eliminar registro                 (disco virtual)\n");
    printf(" 5. Estado del disco RAID 1           (disco espejo)\n");
    printf(" 6. Gestión de procesos               (BSDGames / señales)\n");
    printf(" 7. Nueva partida Snake               (juego)\n");
    printf(" 8. Continuar partida                 (autoguardado)\n");
    printf(" 9. Consultar / modificar matriz      (protección)\n");
    printf("10. Ver bitácora                      (solo admin)\n");
    printf("11. Gestionar usuarios                (solo admin)\n");
    printf("12. Cambiar mi contraseña             (perfil)\n");
    printf("13. Información de virtualización     (hardware / VM)\n");
    printf("14. Acerca de MiniOS                  (kernel)\n");
    printf("15. Monitor de memoria                (mejora opcional)\n");
    printf("16. Cerrar sesión                     (volver al login)\n");
    printf(" 0. Apagar MiniOS                     (solo admin)\n");
}

int main(void) {
    if (mkdir(DIR_DATOS, 0700) != 0) {
        struct stat st;
        if (stat(DIR_DATOS, &st) != 0 || !S_ISDIR(st.st_mode)) {
            perror("No se pudo preparar la carpeta datos");
            return 1;
        }
    }
    (void)chmod(DIR_DATOS, 0700);

    if (fs_montar(DIR_DATOS "minios.disk", DIR_DATOS "minios_espejo.disk") != 0) {
        fprintf(stderr, "No se pudo montar el disco virtual RAID 1.\n");
        return 1;
    }
    seg_cargar_matriz();
    auditar("ARRANQUE", "sistema", "OK");

    for (;;) {
        int login;
        do {
            login = seg_login();
            if (login < 0) {
                auditar("SALIDA", "sistema", "EOF_EN_LOGIN");
                fs_desmontar();
                return 0;
            }
        } while (login == 0);

        int en_sesion = 1;
        while (en_sesion) {
            mostrar_menu();
            int opcion = leer_entero("Opción: ");

            switch (opcion) {
                case 1:
                    if (seg_verificar(OBJ_REGISTROS, D_CREATE)) fs_crear();
                    break;
                case 2:
                    if (seg_verificar(OBJ_REGISTROS, D_READ)) fs_listar();
                    break;
                case 3:
                    if (seg_verificar(OBJ_REGISTROS, D_UPDATE)) fs_actualizar();
                    break;
                case 4:
                    if (seg_verificar(OBJ_REGISTROS, D_DELETE)) fs_borrar();
                    break;
                case 5:
                    if (seg_verificar(OBJ_REGISTROS, D_READ)) fs_estado();
                    break;
                case 6:
                    if (seg_verificar(OBJ_PROCESOS, D_READ)) proc_lanzar_menu();
                    break;
                case 7:
                    if (seg_verificar(OBJ_JUEGOS, D_EXEC)) juego_iniciar();
                    break;
                case 8:
                    if (seg_verificar(OBJ_PARTIDAS, D_READ)) juego_continuar();
                    break;
                case 9:
                    if (seg_verificar(OBJ_MATRIZ, D_READ)) seg_mostrar_matriz();
                    if (seg_es_admin() && seg_verificar(OBJ_MATRIZ, D_UPDATE)) {
                        int modificar = leer_entero("¿Modificar matriz? (1=Sí, 0=No): ");
                        if (modificar == 1) seg_modificar_matriz();
                    }
                    break;
                case 10:
                    if (seg_verificar(OBJ_BITACORA, D_READ)) bitacora_mostrar();
                    break;
                case 11:
                    if (seg_verificar(OBJ_USUARIOS, D_CREATE)) seg_gestionar_usuarios();
                    break;
                case 12:
                    if (seg_verificar(OBJ_USUARIOS, D_UPDATE)) seg_cambiar_clave();
                    break;
                case 13:
                    if (seg_verificar(OBJ_VIRTUALIZACION, D_EXEC)) virt_info();
                    break;
                case 14:
                    if (seg_verificar(OBJ_SISTEMA, D_READ)) acerca_de();
                    break;
                case 15:
                    if (seg_verificar(OBJ_PROCESOS, D_READ)) proc_monitor_memoria();
                    break;
                case 16:
                    seg_cerrar_sesion();
                    en_sesion = 0;
                    break;
                case 0:
                    if (seg_verificar(OBJ_SISTEMA, D_EXEC)) {
                        auditar("APAGADO", "sistema", "OK");
                        fs_desmontar();
                        printf("Apagando MiniOS v%s...\n", VERSION_MINIOS);
                        return 0;
                    }
                    break;
                default:
                    if (feof(stdin)) {
                        auditar("SALIDA", "sistema", "EOF_EN_MENU");
                        fs_desmontar();
                        return 0;
                    }
                    printf("Opción inválida. Seleccione una opción del menú.\n");
                    break;
            }
        }
    }
}
