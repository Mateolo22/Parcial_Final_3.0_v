# MiniOS v3.0 — Taller 2

Proyecto en C que amplía el MiniOS del Taller 1 con control de acceso, auditoría,
consulta de virtualización, autoguardado concurrente, RAID 1 y monitor de memoria.

## Requisitos

- Linux (por el uso de `/proc`, `/sys`, `termios`, señales y `systemd-detect-virt`).
- GCC y Make.
- `pthread` (incluido normalmente con glibc).
- Para la prueba de procesos, el paquete que instala `tetris-bsd`/BSDGames.

## Compilar y ejecutar

```bash
make clean
make
./minios
```

`make clean` elimina únicamente los objetos de compilación y el ejecutable. **No elimina
`datos/`**, para no perder usuarios, matriz, registros, bitácora ni partidas guardadas.

## Usuarios iniciales

Cuando `datos/usuarios.dat` no existe o está vacío, MiniOS crea estas cuentas:

| Usuario | Contraseña inicial | Rol |
|---|---|---|
| `admin` | `admin123` | Administrador |
| `operador` | `oper123` | Operador |
| `invitado` | `inv123` | Invitado |

Cambie las contraseñas iniciales desde **Cambiar mi contraseña**. Las contraseñas no
se almacenan en texto plano: el archivo conserva nombre, sal, hash, rol, intentos fallidos
y estado de bloqueo. El hash implementado (djb2 con sal) es didáctico para el taller;
no debe considerarse equivalente a Argon2, bcrypt o yescrypt en un sistema de producción.

## Mejoras implementadas

### 6. Control de acceso

- Login obligatorio antes de mostrar el menú y cierre de sesión que vuelve al login.
- Entrada de contraseña sin eco en terminal interactiva.
- Hash con sal por usuario; archivo `usuarios.dat` con modo `0600`.
- Mensaje genérico para usuario inexistente, clave incorrecta o cuenta bloqueada.
- Bloqueo persistente después de tres intentos fallidos; solo admin puede desbloquear.
- Administración de usuarios y cambio de contraseña con mínimo seis caracteres.
- La matriz se valida en cada acción y se regenera al detectar un archivo antiguo/incompatible. Todos los roles pueden actualizar su propia contraseña (`D_UPDATE` de usuarios); solo admin puede listar, crear o desbloquear cuentas. Los derechos de bitácora, alta de usuarios y modificación de matriz son exclusivos del administrador: la matriz rechaza delegarlos y los elimina si un archivo editado a mano los trae.

### 7. Bitácora

`datos/bitacora.log` registra fecha/hora, usuario, rol, acción, objeto y resultado. Se
registran arranque, apagado, login, logout, permisos concedidos/denegados, cambios de
usuarios y cambios de matriz. Solo admin puede consultar la bitácora. El resumen muestra
accesos denegados, logins fallidos y el usuario con más denegaciones. Al llegar a cinco
denegaciones en una sesión se muestra y registra una alerta una sola vez por sesión.

Para observar el registro en otra terminal:

```bash
tail -f datos/bitacora.log
```

### 8. Virtualización

El módulo consulta CPUID en arquitecturas x86 para identificar fabricante de CPU, bit
hypervisor, firma del hipervisor, VT-x/VMX y AMD-V/SVM. También consulta las banderas EPT
 y NPT de `/proc/cpuinfo`, DMI en `/sys/class/dmi/id/` y `systemd-detect-virt`. En equipos
ARM donde CPUID no está disponible, se informa esa limitación y se siguen consultando DMI
y systemd.

**Informe para completar con la VM que use el grupo:**

1. **Hipervisor y tipo:** ejecute MiniOS y copie `systemd-detect-virt`, firma CPUID y DMI.
   VirtualBox/VMware Workstation suelen ser hipervisores tipo 2 (alojados); KVM, Xen y
   ESXi tienen arquitectura tipo 1/bare-metal según su despliegue.
2. **Método de implementación:** explique si la plataforma expone virtualización asistida
   por hardware (Intel VT-x/AMD-V), emulación o paravirtualización. El dato debe confirmarse
   con la configuración real de la VM y la salida del módulo.
3. **Diferencia de `grep -cE 'vmx|svm' /proc/cpuinfo`:** en el anfitrión se ven las
   capacidades del procesador físico; dentro de la VM solo aparecen las capacidades que
   el hipervisor decide exponer al invitado. Por eso el conteo puede ser menor o cero.
4. **VM frente a contenedor:** una VM virtualiza hardware y ejecuta su propio kernel; un
   contenedor aísla procesos, pero comparte el kernel del sistema anfitrión.

**Snapshot frente a partida guardada:** el snapshot captura el estado de la máquina virtual
(o del disco y, si se incluye, memoria/dispositivos) en el nivel del hipervisor. La partida
`.sav` guarda únicamente el estado lógico del juego de un usuario, en el nivel de la
aplicación MiniOS. El snapshot permite recuperar el entorno completo; la partida permite
retomar el juego sin restaurar el sistema operativo invitado.

Para la evidencia solicitada, tome un snapshot desde VirtualBox/VMware, elimine la carpeta
`datos/` de la VM, arranque MiniOS para evidenciar que se recrea y luego restaure el snapshot.
No elimine datos importantes fuera de una VM desechable o una copia de prueba.

### 9.1 Autoguardado concurrente

- Un hilo guarda la partida del usuario cada 15 segundos.
- El estado compartido se copia bajo `pthread_mutex_t`; el juego no queda bloqueado por las
  operaciones de E/S.
- Un segundo mutex serializa la escritura del archivo: el autoguardado y el guardado manual
  no pueden abrir a la vez el mismo `.tmp`, así que ningún `rename` borra el temporal del otro.
- El guardado usa archivo temporal, `fsync` y `rename` para sustituir el `.sav` de forma
  atómica. Las partidas tienen permisos `0600`.
- Al salir normalmente se guarda el último estado y se puede continuar desde el menú.

**¿Qué ocurriría sin mutex?** El hilo podría copiar la estructura mientras el juego cambia
la posición de la serpiente, puntaje o longitud. La copia podría mezclar valores de dos
instantes distintos, producir un archivo inconsistente o provocar una condición de carrera
(indefinida en C). El mutex hace que la copia vea un estado coherente.

Para probar la recuperación ante `kill -9`, inicie Snake, juegue durante al menos 15
segundos para que se ejecute un autoguardado y termine el proceso con `kill -9 <PID>` desde
otra terminal. Al volver a iniciar MiniOS, seleccione **Continuar partida**. Se recupera
el último estado que alcanzó a guardarse; los movimientos posteriores al último guardado
se pueden perder.

### 9.2 Disco espejo — RAID 1

Cada escritura del disco virtual actualiza `datos/minios.disk` y
`datos/minios_espejo.disk`. Al montar, MiniOS valida el superbloque de ambos. Si falta o
está dañado uno de ellos, copia el disco sano sobre el defectuoso y muestra el aviso de
reconstrucción. Los dos archivos tienen permisos `0600`.

Prueba: cree uno o más registros, apague MiniOS, haga una copia de seguridad de `datos/`,
elimine `datos/minios.disk` y arranque de nuevo. El programa debe reconstruir el disco
principal desde `minios_espejo.disk`; compruebe que los registros siguen visibles. Restaure
la copia de seguridad al finalizar la prueba si es necesario.

### 9.3 Mejora opcional: monitor de memoria

Se eligió el monitor de memoria. La opción **Monitor de memoria** lee `VmSize` (memoria
virtual) y `VmRSS` (memoria residente/física aproximada) desde `/proc/<pid>/status`, obtiene
el tamaño de página con `sysconf(_SC_PAGESIZE)` y calcula cuántas páginas representan esos
valores. Las métricas solo están disponibles mientras el proceso exista.

### Robustez de la terminal y apagado

MiniOS guarda la configuración original del terminal antes de desactivar el eco o entrar en
modo de juego. Si recibe `SIGINT` o `SIGTERM`, restaura el terminal, marca el disco como
limpio y sale con código `128 + señal`: la shell no queda sin eco ni el arreglo RAID en
estado sucio. Dentro de Snake, `Ctrl+C` y `Ctrl+Z` pausan y guardan la partida en lugar de
matar el proceso.

## Archivos de datos y permisos

La carpeta `datos/` se crea con permisos `0700`. Los archivos sensibles (`usuarios.dat`,
`matriz.dat`, discos, bitácora y partidas) se crean/protegen con permisos `0600`.

```bash
ls -l datos/
xxd datos/usuarios.dat | head
```

`xxd` puede mostrar nombres, roles y hashes binarios, pero no debe revelar las contraseñas
iniciales en texto legible.

## Prueba final sugerida

1. `make clean && make` y comprobar que no aparezcan advertencias.
2. Entrar como admin, cambiar la contraseña inicial y crear un usuario de prueba.
3. Fallar tres veces el login del usuario y comprobar que queda bloqueado; desbloquearlo
   con admin.
4. Entrar como invitado e intentar abrir bitácora, gestionar usuarios y apagar MiniOS.
   Debe aparecer `ACCESO DENEGADO` y el sistema debe seguir funcionando.
5. Abrir `tail -f datos/bitacora.log` en otra terminal y observar eventos en vivo.
6. Consultar virtualización y explicar cada dato de la salida.
7. Probar autoguardado, continuar partida y recuperación RAID 1.
8. Lanzar un proceso, suspenderlo con Ctrl+Z, reanudarlo y terminarlo; consultar el monitor
   de memoria.
9. En el login (o en el menú) pulsar `Ctrl+C` y comprobar que la terminal recupera el eco y
   que el siguiente arranque no advierte un apagado sucio.

> La demostración de snapshot debe hacerse dentro de una VM de prueba y con copia de
> seguridad de los archivos que se necesiten conservar.
