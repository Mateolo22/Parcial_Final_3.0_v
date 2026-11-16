# Entrega — MiniOS v3.0 (Taller 2)

Paquete de revisión, corrección y pruebas de MiniOS v3.0. Reúne el código corregido, la suite de pruebas, las capturas de la última ejecución y la documentación de los cinco defectos críticos.

**Estado verificado:** 5/5 críticos corregidos · **136/136** comprobaciones · **11/11** etapas · **59** capturas.

---

## Contenido

```text
entrega-mini-os-v3.0/
├── README.md                         Este índice
├── docs/
│   ├── 01-correccion-5-criticos.md   Qué se corrigió, por qué y evidencia
│   └── 02-guia-pruebas-manuales.md   Guía paso a paso para probar todo
├── capturas/                         58 capturas de pasos + resumen final
├── tests/                            Suite automática (11 etapas)
└── proyecto/                         Copia del código corregido
    ├── Makefile
    ├── minios.c
    ├── README.md
    ├── include/
    ├── src/
    └── datos/                        Solo .gitkeep; se llena al ejecutar
```

### Documentos

- **[`docs/01-correccion-5-criticos.md`](docs/01-correccion-5-criticos.md):** síntoma, causa raíz, corrección, propósito y evidencia de cada crítico (CR-01 a CR-05).
- **[`docs/02-guia-pruebas-manuales.md`](docs/02-guia-pruebas-manuales.md):** 39 casos manuales (T01–T38 + T15b) con pasos, resultados esperados y tabla para registrar evidencias.

### Capturas

`capturas/` contiene la corrida completa en una consola maximizada (1920×1080): una imagen por comando/paso, numerada y con nombre descriptivo. La última es el resumen de la suite (`captura-59-resumen-final-de-la-suite.png`). Las imágenes muestran el comando, la entrada, la pantalla real de MiniOS y el pie `[CAPTURA N]`.

### Tests

`tests/` incluye:

| Archivo | Uso |
| --- | --- |
| `run_all.py` | Suite de 11 etapas y 136 comprobaciones |
| `sav_race_shim.c` | Shim `LD_PRELOAD` para reproducir la carrera del `.sav.tmp` |
| `run_visible.ps1` | Visor Windows que maximiza la consola y captura cada paso |
| `screenshot.ps1` | Captura de la ventana por título (`PrintWindow` + DWM) |
| `README.md` | Uso detallado de la suite |

---

## Inicio rápido

### 1. Compilar y ejecutar

Requisitos: Linux (nativo, WSL2 o VM), `gcc`, `make`, `pthread` y `python3`.

```bash
cd proyecto
make clean
make
./minios
```

Credenciales iniciales:

| Usuario | Contraseña | Rol |
| --- | --- | --- |
| `admin` | `admin123` | Administrador |
| `operador` | `oper123` | Operador |
| `invitado` | `inv123` | Invitado |

Cambia las contraseñas desde el menú `12`. Si `datos/` no existe, MiniOS crea las cuentas, la matriz y el RAID en el primer arranque.

### 2. Ejecutar la suite automática

Desde la raíz de esta entrega:

```bash
MINIOS_PROJECT="$PWD/proyecto" python3 tests/run_all.py --stage all
```

Resultado esperado:

```text
RESULTADO: 136/136 comprobaciones correctas
```

Si ejecutas desde otra ubicación, ajusta la ruta:

```bash
MINIOS_PROJECT=/ruta/a/entrega-mini-os-v3.0/proyecto \
  python3 /ruta/a/entrega-mini-os-v3.0/tests/run_all.py --stage all
```

Una etapa concreta:

```bash
MINIOS_PROJECT="$PWD/proyecto" python3 tests/run_all.py --stage signals
MINIOS_PROJECT="$PWD/proyecto" python3 tests/run_all.py --stage race
MINIOS_PROJECT="$PWD/proyecto" python3 tests/run_all.py --stage rbac
```

La suite no modifica `proyecto/datos/`: cada etapa trabaja en `/tmp/minios-suite/<etapa>`. La etapa `build` compila con `-Werror`; las demás asumen que `proyecto/minios` ya existe (ejecuta `build` o `make` primero).

### 3. Pruebas manuales

Sigue [`docs/02-guia-pruebas-manuales.md`](docs/02-guia-pruebas-manuales.md). Empieza por T01 (compilación) y T02 (primer arranque); no necesitas la suite para nada de la guía.

---

## Los cinco críticos

| ID | Defecto | Corrección | Evidencia |
| --- | --- | --- | --- |
| CR-01 | `Ctrl+C` deja el terminal sin eco y el RAID sucio | `terminal.c/h` + manejador de señales | Etapa `signals`; capturas 32–35, 38 |
| CR-02 | Carrera entre autoguardado y guardado manual | `mutex_guardado` en `juego.c` | Etapa `race`; captura 50 |
| CR-03 | RBAC permite delegar derechos exclusivos y no normaliza la matriz | Rechazo y normalización en `seguridad.c` | Etapa `rbac`; capturas 14–20 |
| CR-04 | La firma del hipervisor siempre es `no disponible` | CPUID directo en `virtualizacion.c` | Etapa `base`; captura 07 |
| CR-05 | `cpu_vendor` sin terminador NUL | Buffer de 16 bytes y NUL explícito | Inspección + build `-Werror` |

El detalle completo, con causa raíz, propósito y comandos de reproducción, está en [`docs/01-correccion-5-criticos.md`](docs/01-correccion-5-criticos.md).

---

## Notas

- **Código:** `proyecto/` es una copia del proyecto corregido. El proyecto de trabajo original no se modifica al usar esta entrega.
- **Datos:** `proyecto/datos/` solo contiene `.gitkeep`. No se incluyen usuarios, partidas ni bitácoras de las pruebas; se crean al arrancar.
- **Windows:** `tests/run_visible.ps1` es específico del entorno original (contenedor `minios-dev`, `conhost`, Docker). Para probar en Linux usa `run_all.py`; las capturas ya están generadas.
- **Seguridad:** el hash de contraseñas es didáctico (djb2 con sal), no un estándar de producción.
- **Privacidad:** las capturas no contienen datos personales; usan las cuentas y registros de prueba del taller.

---

## Verificación realizada

- Compilación estricta: `-std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Werror -Iinclude -pthread`.
- Suite completa: **136/136** comprobaciones, 11 etapas, código de salida 0.
- Capturas: **59 PNG** (58 pasos + resumen) a 1920×1080, con la consola maximizada.
- Entorno: Ubuntu 24.04 en contenedor, GCC 13.3, PTY real.

Si algo no coincide al ejecutarlo en tu equipo, revisa primero la sección de solución de problemas de la guía manual y el entorno (VM/contenedor, `tetris-bsd`, permisos de `datos/`).
