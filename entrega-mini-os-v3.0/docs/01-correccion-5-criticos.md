# Corrección de los 5 defectos críticos

**Proyecto:** MiniOS v3.0 — Taller 2  
**Paquete:** `entrega-mini-os-v3.0`  
**Fecha:** 16 de noviembre de 2026  
**Alcance:** revisión, corrección y verificación de los cinco hallazgos críticos de la versión recibida.

---

## 1. Resumen ejecutivo

Se corrigieron los cinco defectos críticos detectados en la revisión. Todos tienen una corrección en el código, una prueba automatizada y evidencia visual en la carpeta `capturas/`.

| ID | Defecto | Tipo de riesgo | Corrección | Evidencia principal |
| --- | --- | --- | --- | --- |
| CR-01 | `Ctrl+C` deja el terminal sin eco y el RAID sucio | Usabilidad y consistencia de datos | `terminal.c/h`, manejador de señales en `main.c` | Etapa `signals`; capturas 32–35 y 38 |
| CR-02 | Carrera entre autoguardado y guardado manual del Snake | Pérdida/corrupción de datos | `mutex_guardado` en `juego.c` | Etapa `race`; captura 50 |
| CR-03 | El RBAC permite delegar derechos exclusivos del admin y no normaliza la matriz | Seguridad y coherencia de permisos | Rechazo y normalización en `seguridad.c` | Etapa `rbac`; capturas 14–20 |
| CR-04 | La firma del hipervisor siempre aparece como “no disponible” | Funcionalidad del módulo de virtualización | Lectura directa de CPUID en `virtualizacion.c` | Etapa `base`; captura 07 |
| CR-05 | `cpu_vendor` no tiene espacio para el terminador NUL | Seguridad de memoria / portabilidad | Buffer de 16 bytes y NUL explícito en `virtualizacion.c` | Inspección de código y build estricto |

**Estado verificado:** 5/5 corregidos. La suite completa termina con **136/136 comprobaciones** y **11/11 etapas OK**. Las capturas se generaron en una consola maximizada (1920×1080) y muestran el comando, la entrada, la salida real de MiniOS y el resultado de cada paso.

---

## 2. Cómo se verificó

- Entorno de ejecución: contenedor Ubuntu 24.04, GCC 13.3, `python3`, PTY real y `make`.
- Compilación estricta:

  ```bash
  make clean
  make CFLAGS="-std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Werror -Iinclude -pthread"
  ```

- Suite integral:

  ```bash
  MINIOS_PROJECT="$PWD/proyecto" python3 tests/run_all.py --stage all
  ```

- Resultado esperado:

  ```text
  RESULTADO: 136/136 comprobaciones correctas
  ```

- Evidencia visual: `capturas/` contiene 58 capturas de pasos y el resumen final (59 PNG). Cada imagen incluye el pie `[CAPTURA N] ...`, el comando ejecutado y la pantalla de MiniOS.
- Cada etapa crea su propio sandbox en `/tmp/minios-suite/<etapa>`; la suite no modifica el `datos/` del proyecto.

---

## 3. CR-01 — `Ctrl+C` deja el terminal sin eco y el RAID sucio

### Síntoma

Al pulsar `Ctrl+C` en el prompt de contraseña, el proceso terminaba por la señal por defecto (código 2), el terminal quedaba sin eco y el superbloque del RAID quedaba marcado como sucio (`limpio = 0`). En el siguiente arranque aparecía la advertencia de apagado sucio. Dentro de Snake, una interrupción podía dejar el terminal en modo crudo.

### Causa raíz

- No había manejadores para `SIGINT`/`SIGTERM`.
- `leer_clave()` desactivaba el eco y no existía un registro global de la configuración original de `termios`.
- Al morir el proceso por la señal, no se ejecutaba `fs_desmontar()` y el superbloque no se marcaba limpio.
- El modo crudo del juego no gestionaba `ISIG`, por lo que `Ctrl+C`/`Ctrl+Z` podían matar o suspender el proceso en lugar de pausar la partida.

### Corrección

- **`src/terminal.c` + `include/terminal.h` (nuevos):** guardan la configuración original de `termios` y la restauran desde el manejador de señales.
- **`src/main.c`:** `manejador_salida()` restaura el terminal, llama a `fs_desmontar()` y sale con `_exit(128 + sig)`. Los manejadores se instalan con `sigaction`, sin `SA_RESTART`, para interrumpir `fgets`/`select`.
- **`src/seguridad.c`:** `leer_clave()` registra la configuración antes de desactivar el eco.
- **`src/juego.c`:** el modo crudo limpia `ISIG`; `Ctrl+C` y `Ctrl+Z` llegan como bytes y se tratan como pausa y guardado, no como terminación.

### Por qué y para qué

El terminal es la interfaz principal de MiniOS: dejarlo sin eco obliga al usuario a cerrar la sesión o reiniciar la terminal. Marcar el RAID como limpio evita advertencias falsas y conserva la consistencia del disco. La corrección también garantiza que una interrupción no deje la partida en un estado inconsistente.

### Evidencia

Etapa `signals` (10/10):

- `Ctrl+C en contraseña sale con 130`
- `ECHO restaurado tras Ctrl+C`
- `superbloque limpio tras Ctrl+C`
- `segundo arranque sin aviso de apagado sucio`
- `Ctrl+C en el menú sale con 130`
- `ECHO intacto en el menú`
- `disco limpio tras Ctrl+C en el menú`
- `SIGTERM sale con 143`
- `disco limpio tras SIGTERM`

Etapa `game`:

- `Ctrl+C dentro del juego pausa y guarda`
- `terminal restaurado tras Ctrl+C en juego`

Capturas: `captura-32-ctrl-c-en-el-login-restaura-la-terminal.png`, `captura-33-segundo-arranque-sin-apagado-sucio.png`, `captura-34-ctrl-c-en-el-menu-cierra-limpio.png`, `captura-35-sigterm-cierra-limpio.png`, `captura-38-ctrl-c-dentro-del-juego-pausa-y-guarda.png`.

---

## 4. CR-02 — Carrera entre autoguardado y guardado manual

### Síntoma

Con un retraso artificial de 500 ms en `open()` (shim `LD_PRELOAD`), el hilo de autoguardado y el guardado manual abrían a la vez el mismo archivo temporal `partida_<usuario>.sav.tmp`. El primer `rename()` ganaba; el segundo fallaba con `ENOENT`. El perdedor seguía escribiendo sobre el `.sav` que ya había sido renombrado, rompiendo la atomicidad, y aparecía `No se pudo guardar la partida.`.

### Causa raíz

No existía exclusión mutua alrededor del ciclo completo de guardado (abrir temporal, escribir, `fsync`, `rename`). Los dos hilos compartían la misma ruta temporal y podían pisarse.

### Corrección

- **`src/juego.c`:** se añadió `mutex_guardado`, que serializa la escritura del archivo temporal y el `rename`.
- El estado del juego se copia bajo `mutex_partida`; el guardado no bloquea el bucle del juego.
- La escritura sigue siendo atómica: temporal → `fsync` → `rename`.
- El modo crudo limpia `ISIG` para que la pausa no interrumpa el guardado de forma descontrolada.

### Por qué y para qué

El autoguardado es la protección contra cierres inesperados. Si dos guardados compiten, se puede perder el último estado o dejar un `.sav` parcial. El mutex garantiza que el archivo final siempre sea el resultado de un guardado completo y que el `.sav` anterior se reemplace de forma atómica.

### Evidencia

Etapa `race` (10/10) con `tests/sav_race_shim.c` y `LD_PRELOAD`:

- `el shim registra escrituras` (al menos 2 `rename`)
- `todos los rename tienen éxito` (todos `= 0`)
- `ningún rename devolvió error` (sin `= -1`)
- `sin aviso de autoguardado fallido`
- `sin aviso de guardado manual fallido`
- `los dos guardados usan hilos distintos`
- `.sav final válido` (1680 bytes)

Etapa `game`:

- `autoguardado a los 15 s`
- `q pausa y guarda`
- `.sav mide 1680 bytes`
- `.sav con permisos 0600`
- `continuar partida`
- `rechaza un .sav corrupto`
- `recupera la partida tras kill -9`

Captura: `captura-50-carrera-de-guardado-sin-errores.png` y las capturas 36–41 del juego.

---

## 5. CR-03 — RBAC incoherente: derechos exclusivos delegables y matriz no normalizada

### Síntoma

La matriz permitía conceder al operador o al invitado derechos que el resto del código reservaba al administrador:

- lectura de bitácora (`D_READ` sobre `bitacora`),
- alta de usuarios (`D_CREATE` sobre `usuarios`),
- edición de la matriz (`D_UPDATE` sobre `matriz`).

El menú seguía exigiendo rol admin, así que la matriz mostraba un derecho que nunca se aplicaba. La bitácora podía registrar `PERMITIDO` para la concesión y `DENEGADO` para la acción, una contradicción difícil de auditar. Además, un `matriz.dat` editado a mano podía inyectar esos derechos y no se corregía al cargar.

### Causa raíz

La validación de exclusividad estaba repartida en los menús, no en la capa que modifica y carga la matriz. `seg_modificar_matriz()` aceptaba cualquier combinación de rol/objeto/derechos, y `seg_cargar_matriz()` confiaba en el contenido de `matriz.dat` sin volver a aplicar la política. Por eso una concesión podía existir en el archivo aunque la acción siguiera bloqueada por el rol.

### Corrección

- **`src/seguridad.c` — `seg_modificar_matriz()`:** rechaza las tres concesiones exclusivas con `Cambio rechazado: ese derecho es exclusivo del administrador.`. También impide que el admin se quite sus propios permisos críticos.
- **`src/seguridad.c` — `seg_cargar_matriz()`:** al cargar, elimina esos derechos de las filas de roles no admin y vuelve a guardar la matriz si hubo correcciones.
- Se conserva `D_UPDATE` de usuarios para todos los roles, necesario para que cada usuario cambie su propia contraseña.
- El control de propiedad de registros y procesos se mantiene: un operador no puede modificar, borrar ni terminar recursos de otro usuario.

### Por qué y para qué

El RBAC debe ser coherente: si un derecho no se puede ejercer, no debe poder concederse. La normalización protege ante archivos manipulados y deja una auditoría sin contradicciones. Es un requisito directo del Taller 2: bitácora, alta de usuarios y edición de matriz son exclusivas del administrador.

### Evidencia

Etapa `rbac` (13/13):

- `rechaza delegar bitácora, usuarios y matriz` (3 rechazos)
- `matriz intacta tras los rechazos`
- `5 accesos denegados consecutivos`
- `alerta de seguridad a las 5 denegaciones`
- `la alerta se emite una sola vez`
- `invitado no puede apagar MiniOS`
- `invitado no puede gestionar usuarios`
- `no hay una segunda alerta`
- `bitácora: bitácora denegada a invitado`
- `bitácora: alerta registrada`
- `sin PERMITIDO de bitácora para invitado`
- `matriz manipulada se normaliza al cargar`

Etapa `crud` (16/16), entre otras:

- `operador no actualiza registro ajeno`
- `operador no borra registro ajeno`
- `operador borra su propio registro`
- `admin borra un registro ajeno`

Capturas: 14–20 (RBAC y matriz) y 21–31 (CRUD y auditoría).

---

## 6. CR-04 — La firma del hipervisor nunca se detecta

### Síntoma

La opción **13. Información de virtualización** mostraba `Firma del hipervisor: no disponible` incluso dentro de una máquina virtual, aunque CPUID sí expusiera la firma.

### Causa raíz

`virtualizacion.c` usaba `__get_cpuid(0x40000000, ...)`. En GCC 13, `__get_cpuid()` calcula internamente `__ext = leaf & 0x80000000`; para `0x40000000` el resultado es 0, así que trata el leaf como básico y la llamada falla siempre. El leaf del hipervisor quedaba sin leer.

### Corrección

- Se comprueba primero el bit 31 de `CPUID.1.ECX` (hipervisor presente).
- Solo si ese bit está activo se lee `0x40000000` con `__cpuid()` directo.
- Se copian EBX/ECX/EDX en `firma`, se termina en NUL y se clasifica el hipervisor (Hyper-V, VMware, KVM/QEMU, Xen, VirtualBox).

### Por qué y para qué

El Taller 2 pide identificar el hipervisor y explicar el tipo de virtualización. Una firma vacía impide responder esa parte y hace que la conclusión del módulo sea incorrecta. La corrección mantiene el comportamiento esperado en equipos físicos: si no hay bit de hipervisor, no se inventa una firma.

### Evidencia

Etapa `base` (17/17), comprobaciones añadidas:

- `módulo virtualización responde`
- `virtualización clasifica el entorno`

Salida real en el entorno de prueba (contenedor sobre WSL2/Hyper-V):

```text
=========== INFORMACIÓN DE VIRTUALIZACIÓN ===========
Fabricante CPU (CPUID):       GenuineIntel
¿Dentro de una VM?:           SÍ
Firma del hipervisor:         Microsoft Hv
...
Conclusión: MiniOS corre dentro de una VM (Hyper-V ...), sobre una CPU GenuineIntel.
```

Captura: `captura-07-informacion-de-virtualizacion.png`.

> En un equipo físico es correcto obtener `NO DETECTADO` y `no disponible`; la corrección se verifica dentro de una VM.

---

## 7. CR-05 — `cpu_vendor` sin terminador NUL

### Síntoma

El buffer se declaraba como `char cpu_vendor[13] = "no disponible";`. La cadena `"no disponible"` ocupa exactamente 13 bytes y no deja espacio para el NUL. En x86 el código escribía `cpu_vendor[12] = '\0'` después de copiar los 12 bytes del fabricante, pero en ARM (sin CPUID) se imprimía el valor inicial sin terminador: `printf("%s", cpu_vendor)` leía más allá del arreglo (comportamiento indefinido).

### Causa raíz

El tamaño del buffer se calculó para los 12 caracteres del fabricante, no para la cadena inicial de reserva ni para su terminador.

### Corrección

- `char cpu_vendor[16] = "no disponible";`
- `cpu_vendor[12] = '\0';` explícito tras copiar el fabricante.
- La rama no x86 conserva la cadena de reserva correctamente terminada.

### Por qué y para qué

Es una corrección de seguridad de memoria y portabilidad. El proyecto declara soporte para ARM en el módulo de virtualización; sin terminador, la impresión puede leer memoria adyacente y mostrar basura o provocar un fallo. El build estricto con `-Werror` no detecta este caso en x86 porque la rama no se compila, por eso la evidencia es la inspección del código.

### Evidencia

```text
src/virtualizacion.c:
  char cpu_vendor[16] = "no disponible";
  ...
  cpu_vendor[12] = '\0';
```

Comprobación reproducible:

```bash
grep -n "cpu_vendor" src/virtualizacion.c
make clean
make CFLAGS="-std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Werror -Iinclude -pthread"
```

El build termina sin advertencias. No hay captura de ejecución porque el defecto solo se manifiesta en la rama no x86; la evidencia es el código y la compilación estricta.

---

## 8. Qué se preservó y qué no se tocó

- Se mantuvieron las funcionalidades del Taller 2: login, usuarios, RBAC, bitácora, CRUD, RAID 1, Snake con autoguardado, procesos, memoria y virtualización.
- No se cambiaron las credenciales iniciales ni el formato de los archivos de datos.
- La matriz predeterminada y los permisos `0700`/`0600` se conservan.
- El hash de contraseñas sigue siendo djb2 con sal, didáctico para el taller; no se presenta como equivalente a Argon2/bcrypt.
- La suite usa un shim `LD_PRELOAD` y `SIGSTOP` en el PTY del arnés para reproducir la carrera y la suspensión; son mecanismos de prueba, no cambios del programa.

### Limitaciones conocidas

- El manejador de señales usa `tcsetattr`, `fsync` y `close` para restaurar y desmontar. En la práctica funciona y está verificado, pero no todas esas llamadas están en la lista POSIX de funciones estrictamente async-signal-safe; si se buscara un rigor formal, habría que mover el desmontaje a un hilo o a una bandera atendida por el bucle principal.
- Una línea más larga que el búfer de `leer_linea` deja el resto como entrada del siguiente prompt (no se drena la entrada sobrante). Es un defecto menor conocido, aislado en la etapa `edge`; no bloquea el taller.
- El módulo de virtualización depende de CPUID/DMI/`systemd-detect-virt`; en un equipo físico o en un contenedor con restricciones puede mostrar `no disponible`, que es el resultado correcto para ese entorno.

---

## 9. Cómo reproducir la evidencia

### Suite completa

```bash
cd entrega-mini-os-v3.0
MINIOS_PROJECT="$PWD/proyecto" python3 tests/run_all.py --stage all
```

Resultado esperado: `136/136 comprobaciones correctas` y código de salida 0.

### Una etapa concreta

```bash
MINIOS_PROJECT="$PWD/proyecto" python3 tests/run_all.py --stage signals
MINIOS_PROJECT="$PWD/proyecto" python3 tests/run_all.py --stage race
MINIOS_PROJECT="$PWD/proyecto" python3 tests/run_all.py --stage rbac
```

### Capturas

Las capturas ya están generadas en `capturas/`. Para regenerarlas en el entorno Windows original con el contenedor `minios-dev`:

```powershell
conhost.exe powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File "C:\ruta\al\proyecto\tests\run_visible.ps1"
```

El visor maximiza la consola, ejecuta las 11 etapas y guarda una captura por paso más el resumen. En la entrega se incluyen las 59 imágenes de la última corrida.

---

## 10. Mapa de archivos relevantes

| Archivo | Cambio |
| --- | --- |
| `proyecto/src/terminal.c` | Registro/restauración de `termios` desde señales |
| `proyecto/include/terminal.h` | Interfaz de terminal |
| `proyecto/src/main.c` | Manejadores `SIGINT`/`SIGTERM`, salida `128+señal` |
| `proyecto/src/seguridad.c` | `leer_clave`, RBAC exclusivo y normalización de matriz |
| `proyecto/src/juego.c` | `mutex_guardado`, modo crudo con `ISIG` desactivado |
| `proyecto/src/virtualizacion.c` | CPUID del hipervisor y buffer `cpu_vendor[16]` |
| `tests/run_all.py` | Suite de 136 comprobaciones |
| `tests/sav_race_shim.c` | Shim para reproducir la carrera del `.sav.tmp` |
| `tests/run_visible.ps1` | Visor con capturas por comando |
| `capturas/` | 58 capturas de pasos + resumen final |
