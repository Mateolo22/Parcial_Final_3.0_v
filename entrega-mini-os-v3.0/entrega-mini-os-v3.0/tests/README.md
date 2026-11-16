# Suite de pruebas de MiniOS v3.0

> **En esta entrega**, `tests/` está al mismo nivel que `proyecto/`, no dentro de él.
> Desde `tests/`: `MINIOS_PROJECT="$PWD/../proyecto" python3 run_all.py --stage all`.
> Desde la raíz: `MINIOS_PROJECT="$PWD/proyecto" python3 tests/run_all.py --stage all`.

Suite integral de 11 etapas y 136 comprobaciones. Cada etapa crea su propio
sandbox en `/tmp/minios-suite/<etapa>`, así que no modifica el `datos/` del
proyecto.

## Etapas

| Etapa | Qué cubre |
| --- | --- |
| `build` | Compilación estricta (`-Werror`) y artefactos |
| `base` | Arranque, RAID, usuarios iniciales, virtualización y permisos 0700/0600 |
| `login` | 3 fallos, bloqueo persistente, desbloqueo por admin |
| `rbac` | Exclusividad del admin, alerta de 5 denegaciones, matriz manipulada |
| `crud` | Crear/consultar/actualizar/borrar, propiedad y bitácora |
| `signals` | Ctrl+C en login y menú, SIGTERM, terminal y superbloque limpio |
| `game` | Snake: autoguardado, continuar, Ctrl+C, `.sav` corrupto, `kill -9` |
| `raid` | Borrar el disco principal y reconstruir desde el espejo |
| `proc` | Lanzar, suspender, memoria, reanudar, terminar y propiedad |
| `race` | Carrera del `.sav.tmp` con shim `LD_PRELOAD` |
| `edge` | Entradas inválidas, EOF y límites de los buffers |

## Ejecución headless

Desde la raíz del proyecto (Linux/macOS/WSL con `make`, `gcc` y `python3`):

```bash
make clean && make
python3 tests/run_all.py --stage all
```

Si el código de tests vive en otra carpeta, indica el proyecto con
`MINIOS_PROJECT`:

```bash
MINIOS_PROJECT=/ruta/al/proyecto python3 /ruta/a/tests/run_all.py --stage all
```

En el contenedor de desarrollo de este equipo:

```bash
docker exec minios-dev bash -lc "rm -rf /work && cp -r /src /work"
docker exec -e MINIOS_VISUAL=1 -it minios-dev python3 /work/tests/run_all.py --stage all
```

Sin `MINIOS_VISUAL=1` solo se imprimen los resultados. Con `MINIOS_VISUAL=1`
se ve además:

- `$ comando` — el comando que se ejecuta.
- `> entrada` — la tecla o el dato enviado a MiniOS.
- `│ salida` — la pantalla real de MiniOS.

Una etapa concreta:

```bash
docker exec -e MINIOS_VISUAL=1 -it minios-dev python3 /work/tests/run_all.py --stage game
```

## Corrida visible con capturas (Windows)

```powershell
conhost.exe powershell.exe -NoProfile -ExecutionPolicy Bypass -File "C:\...\tests\run_visible.ps1"
```

La ventana ejecuta las 11 etapas y guarda una captura por paso en
`..\capturas-pruebas\` (hermano del proyecto), numeradas y con nombre
descriptivo. Son 58 capturas de pasos más el resumen final. Antes de la
primera captura la consola se maximiza, así que las imágenes salen a la
resolución de la pantalla (por ejemplo 1920×1080) y muestran la ventana
completa con más líneas de contexto. Por ejemplo:

```
captura-01-limpieza-de-artefactos.png
captura-02-compilacion-estricta-con-werror.png
...
captura-58-eof-en-el-login.png
captura-59-resumen-final-de-la-suite.png
```

Cada imagen incluye el comando ejecutado, la entrada enviada a MiniOS
(`> ...`), su salida real y el título `[CAPTURA N] ...` como pie. Al final la
ventana queda abierta con el resumen; no se generan archivos de log.

Para una corrida parcial o automatizada:

```powershell
# solo algunas etapas
...\run_visible.ps1 -Etapas build,login,edge

# sin pausa final (CI o verificación)
...\run_visible.ps1 -NoPause
```

## Credenciales de prueba

`admin/admin123`, `operador/oper123`, `invitado/inv123`.

## Notas

- `sav_race_shim.c` se compila como biblioteca compartida y se carga con
  `LD_PRELOAD` para retrasar 500 ms el `open()` del `.sav.tmp` y forzar la
  carrera entre el hilo de autoguardado y el guardado manual.
- En el PTY del arnés el grupo de procesos queda huérfano y el kernel
  descarta `SIGTSTP`; la etapa `proc` usa `SIGSTOP`, que ejercita el mismo
  camino del programa (`waitpid` con `WUNTRACED` → `SUSPENDIDO` → `SIGCONT`).
- Defecto menor conocido: una línea más larga que el buffer de `leer_linea`
  deja el resto como entrada del siguiente prompt (no se drena la entrada
  sobrante). No es bloqueante; la etapa `edge` aísla esa sesión.
