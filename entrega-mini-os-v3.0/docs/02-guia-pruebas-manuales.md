# Guía de pruebas manuales — MiniOS v3.0

Esta guía permite probar el sistema completo sin depender de la suite automática. Está pensada para ejecutarse en una copia desechable del proyecto o en una máquina virtual, porque algunas pruebas borran discos, bloquean cuentas o matan procesos.

**Proyecto incluido en este paquete:** `proyecto/`  
**Documento de los 5 críticos:** `docs/01-correccion-5-criticos.md`  
**Capturas de referencia:** `capturas/`

---

## Índice

0. [Mapa de los 5 críticos](#0-mapa-de-los-5-críticos-a-las-pruebas)
1. [Preparación](#1-preparación)
2. [Compilación y primer arranque](#2-compilación-y-primer-arranque)
3. [Login, bloqueo y usuarios](#3-login-bloqueo-y-usuarios)
4. [RBAC: permisos y denegaciones](#4-rbac-permisos-y-denegaciones)
5. [CRUD de registros y propiedad](#5-crud-de-registros-y-propiedad)
6. [RAID 1: reconstrucción desde el espejo](#6-raid-1-reconstrucción-desde-el-espejo)
7. [Bitácora y auditoría](#7-bitácora-y-auditoría)
8. [Señales, terminal y apagado limpio](#8-señales-terminal-y-apagado-limpio)
9. [Snake: autoguardado, pausa y recuperación](#9-snake-autoguardado-pausa-y-recuperación)
10. [Procesos y monitor de memoria](#10-procesos-y-monitor-de-memoria)
11. [Virtualización](#11-virtualización)
12. [Entradas inválidas, EOF y límites](#12-entradas-inválidas-eof-y-límites)
13. [Suite automática (opcional)](#13-suite-automática-opcional)
14. [Registro de pruebas](#14-registro-de-pruebas)
15. [Solución de problemas](#15-solución-de-problemas)
16. [Limpieza final](#16-limpieza-final)

---

## 0. Mapa de los 5 críticos a las pruebas

| Crítico | Dónde se prueba |
| --- | --- |
| CR-01: terminal y apagado limpio | T21, T22, T23, T26 |
| CR-02: carrera de guardado | T24, T25, T29 |
| CR-03: RBAC y matriz | T09, T10, T11, T12 |
| CR-04: firma del hipervisor | T33 |
| CR-05: buffer `cpu_vendor` | T01 + inspección de `src/virtualizacion.c` (solo se manifiesta en no-x86) |

El detalle de cada corrección está en `docs/01-correccion-5-criticos.md`.

---

## 1. Preparación

### 1.1 Requisitos

- Linux nativo, WSL2 o una máquina virtual Linux.
- `gcc`, `make`, `python3` y `pthread` (glibc).
- Opcional: paquete `bsdgames` para tener `tetris-bsd`.
- Opcional: `systemd-detect-virt` para el módulo de virtualización.

En Ubuntu/Debian:

```bash
sudo apt update
sudo apt install build-essential python3 bsdgames
```

Si no puedes instalar `bsdgames`, la sección 10 explica cómo crear un `tetris-bsd` de prueba.

### 1.2 Copia de seguridad

MiniOS guarda todo en `datos/`. Antes de probar, haz una copia si esa carpeta ya tiene información:

```bash
cd proyecto
cp -a datos "datos.backup-$(date +%Y%m%d-%H%M%S)"
```

`make clean` **no** borra `datos/`; solo elimina objetos y el ejecutable.

### 1.3 Credenciales iniciales

| Usuario | Contraseña | Rol |
| --- | --- | --- |
| `admin` | `admin123` | Administrador |
| `operador` | `oper123` | Operador |
| `invitado` | `inv123` | Invitado |

Si la carpeta `datos/` no existe, MiniOS la crea con estas cuentas en el primer arranque. Si ya existe, conserva las contraseñas y usuarios anteriores.

### 1.4 Reinicio de fábrica (opcional)

Solo en una copia desechable:

```bash
cd proyecto
rm -rf datos
./minios
```

Al arrancar se crean de nuevo los usuarios iniciales, la matriz y el RAID. **Esto borra todos los registros, partidas y bitácoras.**

---

## 2. Compilación y primer arranque

### T01 — Compilación estricta

```bash
cd proyecto
make clean
make CFLAGS="-std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Werror -Iinclude -pthread"
```

**Resultado esperado:** termina sin errores ni advertencias y genera `./minios`.

### T02 — Primer arranque

```bash
./minios
```

**Resultado esperado:**

- `[RAID 1] Ambos discos no son válidos. Se crea un arreglo nuevo.`
- `Usuarios iniciales creados. Cambie sus claves desde el menú.`
- Prompt `Usuario:`.

**Evidencia sugerida:** captura de la pantalla de arranque y `ls -l datos/`.

### T03 — Permisos de los archivos

En otra terminal, con MiniOS detenido o en ejecución:

```bash
cd proyecto
ls -ld datos
ls -l datos
```

**Resultado esperado:** `datos/` con `drwx------` (0700) y los archivos `usuarios.dat`, `matriz.dat`, `minios.disk`, `minios_espejo.disk`, `bitacora.log` y las partidas con `-rw-------` (0600).

---

## 3. Login, bloqueo y usuarios

### T04 — Login correcto

1. Ejecuta `./minios`.
2. Usuario: `admin`.
3. Contraseña: `admin123`.

**Resultado esperado:** aparece el menú con `MiniOS v3.0 | admin (admin)` y las 16 opciones.

### T05 — Contraseña incorrecta y cuenta inexistente

1. Cierra sesión con `16`.
2. Usuario: `admin`.
3. Contraseña: `clave-mala`.
4. Repite con un usuario que no exista, por ejemplo `nadie`.

**Resultado esperado:** en ambos casos, `Usuario o contraseña incorrectos.` (mensaje genérico, sin revelar si el usuario existe).

### T06 — Bloqueo tras tres fallos

1. En el login, usuario `invitado`, contraseña incorrecta. Repite **tres veces**.
2. Entra como `admin`.
3. Menú `11` → `1` (listar usuarios).

**Resultado esperado:** la fila de `invitado` muestra `3` fallos y `BLOQUEADO`. Un cuarto intento con la contraseña correcta sigue mostrando el mensaje genérico.

### T07 — Desbloqueo por admin

1. Como admin: menú `11` → `3`.
2. Usuario a desbloquear: `invitado`.
3. Cierra sesión (`16`) y entra como `invitado` con `inv123`.

**Resultado esperado:** `Usuario desbloqueado.` y el invitado puede entrar.

### T08 — Crear usuario y cambiar contraseña

1. Como admin: menú `11` → `2`.
2. En `Nombre (letras, números, _ o -):` escribe `prueba`.
3. Contraseña: `prueba123` (mínimo 6).
4. Rol: `1` (operador).
5. Cierra sesión y entra como `prueba`/`prueba123`.
6. Menú `12`, contraseña actual `prueba123`, nueva `nueva123`.
7. Cierra sesión y entra con la nueva.

**Resultado esperado:** `Usuario creado correctamente.` y `Contraseña actualizada.`. Si la contraseña actual es incorrecta, aparece `Contraseña actual incorrecta.`.

> La gestión de usuarios (listar/crear/desbloquear) es solo para admin; el cambio de la propia contraseña está disponible para todos los roles.

---

## 4. RBAC: permisos y denegaciones

### T09 — Intentos del invitado

Entra como `invitado` y prueba:

| Opción | Acción | Resultado esperado |
| --- | --- | --- |
| `10` | Ver bitácora | `ACCESO DENEGADO` |
| `11` | Gestionar usuarios | `ACCESO DENEGADO` |
| `0` | Apagar MiniOS | `ACCESO DENEGADO` |
| `13` | Información de virtualización | `ACCESO DENEGADO` |
| `1` | Crear registro | `ACCESO DENEGADO` |

**Resultado esperado:** MiniOS sigue funcionando después de cada denegación.

### T10 — Alerta a las cinco denegaciones

En la misma sesión de `invitado`, pulsa `10` cinco veces.

**Resultado esperado:** cada intento muestra `ACCESO DENEGADO`; en el quinto aparece una sola vez:

```text
[ALERTA SEGURIDAD] invitado acumula 5 accesos denegados en esta sesión.
```

Una sexta denegación **no** repite la alerta.

### T11 — Matriz y derechos exclusivos

1. Entra como `admin`.
2. Menú `9` (muestra la matriz).
3. Responde `1` a `¿Modificar matriz?`.
4. Prueba las tres concesiones prohibidas:

| Rol | Objeto | Derechos | Resultado esperado |
| --- | --- | --- | --- |
| `1` (operador) | `7` (bitácora) | `2` (leer) | `Cambio rechazado: ese derecho es exclusivo del administrador.` |
| `1` (operador) | `6` (usuarios) | `1` (crear) | El mismo rechazo |
| `2` (invitado) | `4` (matriz) | `4` (actualizar) | El mismo rechazo |

5. Vuelve a mostrar la matriz (`9`): debe seguir intacta.

5. Repite `9` → `1` para cada fila; tras un rechazo, MiniOS vuelve al menú principal.

**Tabla de derechos:** `1` crear, `2` leer, `4` actualizar, `8` borrar, `16` ejecutar. Se pueden combinar, por ejemplo `15` = crear+leer+actualizar+borrar.

### T12 — Matriz manipulada (prueba avanzada)

Solo en una copia desechable:

1. Detén MiniOS.
2. Edita el byte 16 de `datos/matriz.dat` para dar lectura de bitácora al operador:

   ```bash
   python3 - <<'PY'
   from pathlib import Path
   p = Path("datos/matriz.dat")
   b = bytearray(p.read_bytes())
   b[16] = 2          # operador + bitácora D_READ
   p.write_bytes(b)
   PY
   ```

3. Arranca MiniOS y consulta la matriz como admin (`9`).

**Resultado esperado:** el byte vuelve a `0`; la matriz normaliza el derecho al cargar y no permite al operador leer la bitácora.

> Si no quieres editar archivos binarios, esta prueba es opcional. La suite automática la cubre en la etapa `rbac`.

---

## 5. CRUD de registros y propiedad

### T13 — Crear y listar como admin

1. Admin → menú `1`.
2. Nombre: `DocAdmin`.
3. Descripción: `documento del admin`.
4. Menú `2`.

**Resultado esperado:** `Registro ID 1 creado en la ranura 0 (principal + espejo).` y la tabla muestra `DocAdmin` con propietario `admin`.

### T14 — El operador no puede tocar registros ajenos

1. Cierra sesión y entra como `operador`.
2. Menú `3` (actualizar), ID `1`.
3. Menú `4` (borrar): no llegará a pedir el ID.

**Resultado esperado:**

- En `3`: `ACCESO DENEGADO: no es propietario del registro.` (el operador sí tiene `D_UPDATE`, pero no es dueño).
- En `4`: `ACCESO DENEGADO` (el operador **no** tiene `D_DELETE` por defecto; la denegación es por rol, todavía no por propiedad).

### T15 — El operador gestiona su propio registro

1. Como operador: menú `1`, nombre `DocOper`, descripción `documento del operador`.
2. Menú `3`, actualiza su ID: nombre `DocOper2`, descripción `actualizado`.
3. Menú `2`: debe ver su registro actualizado.

**Resultado esperado:** creación y actualización correctas; el registro del admin sigue intacto. El borrado propio se prueba en T15b, después de delegar `D_DELETE`.

### T15b — Delegar el borrado y probar la propiedad

El operador no tiene `D_DELETE` por defecto. Para que la negativa venga del control de propiedad y no de la falta del derecho, el admin lo concede:

1. Cierra sesión y entra como `admin`.
2. Menú `9` → `1` (modificar matriz).
3. Rol: `1` (operador). Objeto: `0` (registros). Nuevos derechos: `15` (crear+leer+actualizar+borrar).
4. Debe aparecer `Matriz actualizada y guardada.`
5. Cierra sesión y entra como `operador`.
6. Menú `4`, intenta borrar el ID del registro del admin (`DocAdmin`): `ACCESO DENEGADO: no es propietario del registro.`
7. Menú `4`, borra su propio registro (`DocOper2`): `Registro ... eliminado. Su ranura queda libre para reutilizarse.`

**Por qué se hace así:** sin la delegación, el menú `4` se deniega antes de llegar a la comprobación de propiedad. La suite automática hace exactamente esta delegación en la etapa `crud`.

### T16 — El admin puede borrar cualquier registro

1. Entra como admin.
2. Menú `4`, introduce el ID del registro `DocAdmin` (consúltalo en `2` si no lo recuerdas).
3. Menú `2`.

**Resultado esperado:** el registro desaparece y la ranura queda libre.

---

## 6. RAID 1: reconstrucción desde el espejo

### T17 — Reconstrucción del disco principal

1. Como admin, crea un registro `DocRAID` (menú `1`).
2. Apaga con `0` (debe mostrar `Apagando MiniOS v3.0...`).
3. En la shell:

   ```bash
   cd proyecto
   rm datos/minios.disk
   ./minios
   ```

4. **Resultado esperado al arrancar:**

   ```text
   [RAID 1] Disco principal ausente o dañado. Reconstruyendo desde el espejo...
   [RAID 1] Disco principal reconstruido.
   ```

5. Entra como admin y consulta los registros (`2`).

**Resultado esperado:** `DocRAID` sigue visible; `datos/minios.disk` existe de nuevo con permisos `0600`.

6. Restaura la copia si la necesitas:

   ```bash
   rm -rf datos
   mv datos.backup-AAAA... datos
   ```

### T18 — Superbloque limpio

Con MiniOS detenido:

```bash
cd proyecto
python3 - <<'PY'
import struct
from pathlib import Path
for nombre in ("minios.disk", "minios_espejo.disk"):
    datos = Path("datos", nombre).read_bytes()
    print(nombre, "limpio =", struct.unpack_from("<I", datos, 20)[0])
PY
```

**Resultado esperado:** ambos muestran `limpio = 1` después de un apagado normal.

---

## 7. Bitácora y auditoría

### T19 — Eventos en vivo

1. En una terminal, dentro de `proyecto/`:

   ```bash
   tail -f datos/bitacora.log
   ```

2. En otra terminal, arranca MiniOS, inicia sesión, crea un registro y cierra sesión.

**Resultado esperado:** el archivo muestra líneas con fecha/hora, usuario, rol, acción, objeto y resultado (`LOGIN EXITOSO`, `CREAR_REGISTRO`, `LOGOUT`, etc.).

### T20 — Resumen de auditoría

1. Como admin, menú `10`.
2. Baja hasta el final de la bitácora.

**Resultado esperado:** aparece `RESUMEN DE AUDITORÍA` con:

- `Accesos denegados:` un número.
- `Intentos de login fallidos:` un número.
- `Usuario con más denegaciones:` un usuario y su conteo.

Los intentos fallidos y las denegaciones de las pruebas anteriores deben aparecer contabilizados.

---

## 8. Señales, terminal y apagado limpio

### T21 — `Ctrl+C` en el login

1. Ejecuta `./minios`.
2. Escribe un usuario y pulsa `Enter` cuando pida la contraseña.
3. Pulsa `Ctrl+C`.
4. En la shell:

   ```bash
   echo $?          # debe ser 130
   stty -a | tr ' ' '\n' | grep -x -- '-echo'
   ```

**Resultado esperado:** el proceso sale con `130`; el terminal **no** queda con `-echo` (el segundo comando no debe imprimir nada). Si vuelves a arrancar, no debe aparecer la advertencia de apagado sucio.

### T22 — `Ctrl+C` en el menú

1. Entra como admin.
2. En el menú, pulsa `Ctrl+C`.

**Resultado esperado:** sale con `130`; el terminal conserva el eco; el siguiente arranque no advierte apagado sucio.

### T23 — `SIGTERM`

1. Arranca MiniOS en una terminal.
2. En otra:

   ```bash
   pgrep -a minios
   kill -TERM <PID>
   ```

3. En la primera terminal, comprueba el código de salida con `echo $?`.

**Resultado esperado:** sale con `143`; el superbloque queda limpio (`1`).

---

## 9. Snake: autoguardado, pausa y recuperación

### T24 — Autoguardado y pausa

1. Admin → menú `7` (Nueva partida Snake).
2. Muévete con `WASD` y espera al menos 15 segundos.
3. Debe aparecer `[Autoguardado realizado: cada 15 segundos]`.
4. Pulsa `q`.

**Resultado esperado:** `Partida pausada y guardada. Puede continuarla desde el menú.` y vuelta al menú.

Comprueba el archivo:

```bash
cd proyecto
stat -c '%s %a' datos/partida_admin.sav
```

**Resultado esperado:** `1680 600` (1680 bytes, permisos 0600).

### T25 — Guardado manual y continuación

1. Menú `7` y pulsa `g` durante la partida.
2. Debe aparecer `Partida guardada manualmente.`
3. Pulsa `q` para pausar.
4. Menú `8` (Continuar partida).

**Resultado esperado:** `Partida recuperada para admin. Puntaje: ...`.

### T26 — `Ctrl+C` dentro del juego

1. Menú `7`.
2. Pulsa `Ctrl+C`.

**Resultado esperado:** `Partida pausada y guardada. Puede continuarla desde el menú.`; el proceso **no** muere y el terminal queda usable. Vuelve al menú.

### T27 — `.sav` corrupto

1. Detén MiniOS.
2. Corrompe la partida:

   ```bash
   printf 'basura' > datos/partida_admin.sav
   ```

3. Arranca, entra como admin y pulsa `8`.

**Resultado esperado:** `La partida guardada no es válida o ya terminó. Inicie una partida nueva.`

### T28 — Game over y récord

1. Menú `7`.
2. Muévete contra el borde hasta chocar (por ejemplo, mantén `w` hasta el borde superior).

**Resultado esperado:** `=== GAME OVER ===`, puntaje final y vuelta al menú. En `2` debe aparecer un registro `Record Snake - admin`.

### T29 — Recuperación tras `kill -9`

1. Menú `7`, juega y espera un autoguardado.
2. En otra terminal:

   ```bash
   pgrep -a minios
   kill -9 <PID>
   ```

3. Arranca de nuevo.

**Resultado esperado:** aparece `ADVERTENCIA: el disco no se desmontó correctamente (apagado sucio).` Entra como admin y pulsa `8`: debe recuperar el último autoguardado. Los movimientos posteriores al guardado pueden perderse; es el comportamiento esperado.

---

## 10. Procesos y monitor de memoria

### T30 — Lanzar, suspender, reanudar y terminar

1. Admin → menú `6` → `1` (Lanzar Tetris/BSDGames).
2. Anota el PID.
3. Pulsa `Ctrl+Z` para suspenderlo.
4. Menú `6` → `2` (listar): debe aparecer `SUSPENDIDO`.
5. Menú `6` → `3`, introduce el PID: `Proceso <PID>: EJECUTANDO.`
6. Vuelve a suspender con `Ctrl+Z`.
7. Menú `6` → `4`, introduce el PID: `Proceso <PID> terminado y recolectado; no queda zombi.`

**Resultado esperado:** la tabla conserva el estado `TERMINADO` y no queda un proceso zombi.

> Si `tetris-bsd` no está instalado, crea un sustituto en una copia desechable:
>
> ```bash
> mkdir -p "$HOME/bin"
> printf '#!/bin/sh\nexec sleep 3600\n' > "$HOME/bin/tetris-bsd"
> chmod +x "$HOME/bin/tetris-bsd"
> export PATH="$HOME/bin:$PATH"
> ./minios
> ```
>
> Si ejecutas MiniOS dentro del PTY del arnés y `Ctrl+Z` no llega, usa `kill -STOP <PID>` desde otra terminal; el programa detecta la suspensión igual.

### T31 — Monitor de memoria

Con un proceso registrado (ejecutando o suspendido):

1. Vuelve al menú principal.
2. Menú `15`.

**Resultado esperado:** `Tamaño de página del sistema`, y por cada proceso:

```text
VmSize: ... kB (... páginas) | VmRSS: ... kB (... páginas)
```

Si el proceso ya terminó, muestra que `/proc` no conserva sus métricas.

### T32 — Propiedad de procesos

1. Como admin, lanza un proceso y suspéndelo.
2. Cierra sesión y entra como `operador`.
3. Menú `6` → `4`, intenta terminar el PID del admin.

**Resultado esperado:** `ACCESO DENEGADO: no es propietario del proceso.` El admin sí puede terminarlo.

---

## 11. Virtualización

### T33 — Información de virtualización

1. Entra como admin.
2. Menú `13`.

**Resultado esperado:** una pantalla con:

- `Fabricante CPU (CPUID):` (por ejemplo `GenuineIntel` o `AuthenticAMD`).
- `¿Dentro de una VM?:` `SÍ` o `NO DETECTADO`.
- `Firma del hipervisor:` una firma (`Microsoft Hv`, `KVMKVMKVM`, `VMwareVMware`, `VBoxVBoxVBox`, `XenVMMXenVMM`) o `no disponible`.
- `Intel VT-x / VMX expuesto:`, `AMD-V / SVM expuesto:`.
- `EPT disponible`, `NPT disponible`.
- `Fabricante DMI`, `Producto DMI`, `systemd-detect-virt`.
- `Tipo de hipervisor:` tipo 1 o tipo 2.
- `Conclusión:`.

**Cómo interpretarlo:**

| Caso | Resultado esperado |
| --- | --- |
| VM con Hyper-V/WSL2 | `SÍ`, firma `Microsoft Hv` |
| VM con VirtualBox | `SÍ`, firma `VBoxVBoxVBox`, tipo 2 |
| VM con KVM/QEMU | `SÍ`, firma `KVMKVMKVM`, tipo 1 |
| Equipo físico | `NO DETECTADO`, firma `no disponible` |

**Evidencia de la corrección CR-04:** en el entorno de prueba aparece `Firma del hipervisor: Microsoft Hv` (antes siempre salía `no disponible`). Ver `capturas/captura-07-informacion-de-virtualizacion.png`.

### T34 — Preguntas del informe

Responde con la salida real de tu equipo:

1. ¿Qué hipervisor y tipo es? (tipo 1 o tipo 2)
2. ¿Expone VT-x/AMD-V? ¿Emulación o paravirtualización?
3. ¿Por qué `grep -cE 'vmx|svm' /proc/cpuinfo` puede dar distinto en el anfitrión y en la VM?
4. ¿Diferencia entre una VM y un contenedor?
5. ¿Diferencia entre un snapshot de la VM y una partida `.sav`?

Las respuestas están desarrolladas en el `README.md` del proyecto; complétalas con los datos de tu máquina.

---

## 12. Entradas inválidas, EOF y límites

### T35 — Opciones inválidas

Como admin, en el menú:

- Escribe `abc` → `Opción inválida. Seleccione una opción del menú.`
- Escribe `999` → el mismo mensaje.
- El menú sigue funcionando.

### T36 — Nombre vacío y límites

1. Menú `1`, pulsa `Enter` sin nombre → `Nombre vacío; operación cancelada.`
2. Menú `1`, nombre de 48 caracteres y descripción de 98 caracteres → se crea correctamente.
3. Para esta prueba usa 48/98 (el caso que cubre la suite). No envíes líneas más largas: el excedente queda en el búfer de entrada y puede aparecer en el siguiente prompt (limitación conocida).

### T37 — Contraseña actual incorrecta

1. Menú `12`.
2. Contraseña actual: `incorrecta`.
3. Nueva contraseña: `nueva123`.

**Resultado esperado:** `Contraseña actual incorrecta.` y la contraseña no cambia.

### T38 — EOF

1. En el menú, pulsa `Ctrl+D`.

**Resultado esperado:** sale con código `0`; la bitácora registra `EOF_EN_MENU`.

2. Arranca de nuevo y pulsa `Ctrl+D` en el prompt `Usuario:`.

**Resultado esperado:** sale con `0`; la bitácora registra `EOF_EN_LOGIN`.

> No uses una línea mucho más larga que el búfer en el login: el resto queda como entrada del siguiente prompt (defecto menor conocido). La etapa `edge` de la suite lo aísla.

---

## 13. Suite automática (opcional)

La suite reproduce todo lo anterior con 136 comprobaciones:

```bash
cd entrega-mini-os-v3.0
MINIOS_PROJECT="$PWD/proyecto" python3 tests/run_all.py --stage all
```

**Resultado esperado:** `136/136 comprobaciones correctas` y código de salida 0.

Para ver los comandos y las pantallas mientras corre:

```bash
MINIOS_PROJECT="$PWD/proyecto" MINIOS_VISUAL=1 python3 tests/run_all.py --stage all
```

Una etapa concreta:

```bash
MINIOS_PROJECT="$PWD/proyecto" python3 tests/run_all.py --stage signals
MINIOS_PROJECT="$PWD/proyecto" python3 tests/run_all.py --stage race
MINIOS_PROJECT="$PWD/proyecto" python3 tests/run_all.py --stage rbac
```

La suite no toca el `datos/` del proyecto: cada etapa usa `/tmp/minios-suite/<etapa>`. No requiere `tetris-bsd` real (compila un proceso falso para la etapa `proc`).

---

## 14. Registro de pruebas

Copia esta tabla en tu informe y complétala con los resultados reales.

| Caso | Qué se probó | Resultado esperado | Resultado obtenido | Evidencia |
| --- | --- | --- | --- | --- |
| T01 | Compilación estricta | Sin advertencias | | |
| T02 | Primer arranque | RAID y usuarios creados | | |
| T03 | Permisos | 0700 / 0600 | | |
| T04 | Login admin | Menú admin | | |
| T05 | Credenciales incorrectas | Mensaje genérico | | |
| T06 | Tres fallos | Cuenta bloqueada | | |
| T07 | Desbloqueo | Invitado puede entrar | | |
| T08 | Usuario y contraseña | Crear y cambiar | | |
| T09 | RBAC invitado | Denegaciones | | |
| T10 | Cinco denegaciones | Alerta única | | |
| T11 | Matriz exclusiva | Tres rechazos | | |
| T12 | Matriz manipulada | Se normaliza | | |
| T13 | CRUD admin | Registro creado | | |
| T14 | Propiedad | Operador denegado | | |
| T15 | CRUD operador | Crear y actualizar lo propio | | |
| T15b | Delegar y borrar | Ajeno denegado; propio borrado | | |
| T16 | Borrado admin | Registro eliminado | | |
| T17 | RAID 1 | Reconstrucción | | |
| T18 | Superbloque | limpio = 1 | | |
| T19 | Bitácora en vivo | Eventos | | |
| T20 | Resumen | Conteos | | |
| T21 | Ctrl+C login | 130, eco, limpio | | |
| T22 | Ctrl+C menú | 130, limpio | | |
| T23 | SIGTERM | 143, limpio | | |
| T24 | Autoguardado | 15 s, .sav 0600 | | |
| T25 | Continuar | Partida recuperada | | |
| T26 | Ctrl+C juego | Pausa y guarda | | |
| T27 | .sav corrupto | Rechazado | | |
| T28 | Game over | Récord | | |
| T29 | kill -9 | Recuperación | | |
| T30 | Procesos | Suspender/reanudar/terminar | | |
| T31 | Memoria | VmSize/VmRSS | | |
| T32 | Propiedad procesos | Denegado | | |
| T33 | Virtualización | Firma/tipo | | |
| T34 | Informe VM | Respuestas | | |
| T35 | Opciones inválidas | Mensaje | | |
| T36 | Límites | Sin ruptura | | |
| T37 | Clave incorrecta | Rechazo | | |
| T38 | EOF | Salida 0 | | |

---

## 15. Solución de problemas

| Síntoma | Causa probable | Solución |
| --- | --- | --- |
| `make` falla con `pthread` | Falta `build-essential` | `sudo apt install build-essential` |
| `No se pudo ejecutar el programa` al lanzar proceso | `tetris-bsd` no está instalado | Instala `bsdgames` o crea el sustituto de la sección 10 |
| `Ctrl+Z` no suspende el proceso | Estás en un PTY sin grupo de primer plano | Usa `kill -STOP <PID>` |
| La virtualización muestra `no disponible` | Equipo físico o VM sin CPUID expuesto | Es correcto; prueba dentro de una VM |
| `ADVERTENCIA: ... apagado sucio` | Se usó `kill -9` o se cortó el proceso | Es esperado; usa `0`, `Ctrl+C` o `SIGTERM` para un cierre limpio |
| `datos/` no se crea | Permisos del directorio | Ejecuta desde una carpeta con escritura; revisa `umask` |
| El terminal queda sin eco | Se mató el proceso con `SIGKILL` | `stty sane` o cierra la terminal; con `Ctrl+C`/`SIGTERM` ya se restaura |
| La suite falla al compilar el shim | Falta `gcc` o `ld` | Instala `build-essential` |
| La suite no encuentra `minios` | Ejecutaste una etapa sin compilar | Ejecuta `make` o la etapa `build` primero |

---

## 16. Limpieza final

1. Cierra MiniOS con `0` (admin) o `16` + `Ctrl+D`.
2. Restaura la copia de seguridad si modificaste datos reales:

   ```bash
   cd proyecto
   rm -rf datos
   mv datos.backup-AAAA... datos
   ```

3. Si cambiaste contraseñas de prueba, vuelve a los valores iniciales en una copia desechable o elimina `datos/` para regenerarlos.
4. No dejes procesos `tetris-bsd` colgados:

   ```bash
   pgrep -a tetris-bsd
   pkill -TERM -x tetris-bsd
   ```

5. Guarda las capturas y este registro en el informe del taller.

---

## Criterios de aceptación

- [ ] Compila con `-Werror` sin advertencias.
- [ ] Los 5 críticos están corregidos y con evidencia.
- [ ] Login, bloqueo, desbloqueo y cambio de contraseña funcionan.
- [ ] RBAC deniega bitácora, usuarios, apagado y virtualización al invitado.
- [ ] Las tres concesiones exclusivas de la matriz se rechazan.
- [ ] El CRUD respeta la propiedad de los registros.
- [ ] El RAID reconstruye el disco principal desde el espejo.
- [ ] `Ctrl+C`/`SIGTERM` restauran el terminal y dejan el disco limpio.
- [ ] Snake autoguarda, pausa, continúa y se recupera tras `kill -9`.
- [ ] Los procesos se suspenden, reanudan y terminan; el monitor muestra memoria.
- [ ] La virtualización identifica el hipervisor dentro de una VM.
- [ ] La suite automática termina en `136/136`.
