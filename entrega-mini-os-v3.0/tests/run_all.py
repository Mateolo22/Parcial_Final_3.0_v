#!/usr/bin/env python3
"""Suite de pruebas de MiniOS v3.0.

Cada --stage es autocontenido: crea su propio sandbox en /tmp/minios-suite,
ejecuta el binario con un PTY real y no toca los datos del proyecto.

Uso:
    python3 tests/run_all.py --stage all
    python3 tests/run_all.py --stage game
"""

import argparse
import os
import pty
import re
import select
import shutil
import signal
import stat
import struct
import subprocess
import sys
import termios
import time
from pathlib import Path

PROJECT = Path(os.environ.get("MINIOS_PROJECT",
                               Path(__file__).resolve().parent.parent)).resolve()
BIN = PROJECT / "minios"
ROOT = Path("/tmp/minios-suite")
TESTS_DIR = Path(__file__).resolve().parent
SHIM_SRC = TESTS_DIR / "sav_race_shim.c"
VISUAL = os.environ.get("MINIOS_VISUAL") == "1"
ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")

pasos = 0
fallos = []


def check(nombre, condicion, detalle=""):
    global pasos
    pasos += 1
    if condicion:
        print(f"  [PASS] {nombre}", flush=True)
    else:
        print(f"  [FAIL] {nombre}", flush=True)
        if detalle:
            for linea in str(detalle).strip().splitlines()[-6:]:
                print(f"         {linea}", flush=True)
        fallos.append(nombre)


def seccion(titulo):
    print(f"\n=== {titulo} ===", flush=True)


def traza(comando):
    """Muestra el comando real que se ejecuta (para la corrida visible)."""
    print(f"$ {comando}", flush=True)


CAPTURAS = os.environ.get("MINIOS_CAPTURAS") == "1"


def captura(descripcion):
    """Pide al visor de Windows una captura de este paso (si está activo)."""
    if CAPTURAS:
        print(f"@@CAPTURA:{descripcion}", flush=True)


def sandbox(nombre):
    destino = ROOT / nombre
    if destino.exists():
        shutil.rmtree(destino)
    destino.mkdir(parents=True)
    return destino


class Mini:
    def __init__(self, trabajo, env_extra=None, path_extra=None, espejo=True):
        entorno = os.environ.copy()
        if env_extra:
            entorno.update(env_extra)
        if path_extra:
            entorno["PATH"] = f"{path_extra}:{entorno.get('PATH', '')}"
        self.buf = b""
        self.pos = 0
        self.status = None
        self.espejo = bool(VISUAL and espejo)
        self._resto = ""
        self._volcadas = 0
        print(f"$ (cd {trabajo} && ./minios)   # sesión PTY", flush=True)
        pid, fd = pty.fork()
        if pid == 0:
            os.chdir(str(trabajo))
            os.environ.clear()
            os.environ.update(entorno)
            os.execv(str(BIN), [str(BIN)])
            os._exit(127)
        self.pid = pid
        self.fd = fd

    def _volcar(self, datos):
        if not self.espejo:
            return
        texto = ANSI_RE.sub("", datos.decode(errors="replace")).replace("\r", "")
        self._resto += texto
        while "\n" in self._resto:
            linea, self._resto = self._resto.split("\n", 1)
            if not linea.strip():
                continue
            if self._volcadas >= 250:
                if self._volcadas == 250:
                    print("  │ [salida truncada para no inundar la terminal]", flush=True)
                self._volcadas += 1
                continue
            print(f"  │ {linea.rstrip()}", flush=True)
            self._volcadas += 1

    def _volcar_resto(self):
        if self.espejo and self._resto.strip():
            print(f"  │ {self._resto.strip()}", flush=True)
        self._resto = ""

    def drain(self, segundos):
        fin = time.monotonic() + segundos
        while time.monotonic() < fin:
            r, _, _ = select.select([self.fd], [], [], 0.05)
            if not r:
                continue
            try:
                datos = os.read(self.fd, 65536)
            except OSError:
                return
            if not datos:
                return
            self.buf += datos
            self._volcar(datos)
            if len(self.buf) > 2_000_000:
                self.buf = self.buf[-1_000_000:]
                self.pos = max(0, self.pos - 1_000_000)

    def wait_for(self, aguja, timeout=12):
        objetivo = aguja.encode() if isinstance(aguja, str) else aguja
        fin = time.monotonic() + timeout
        while True:
            i = self.buf.find(objetivo, self.pos)
            if i >= 0:
                self.pos = i + len(objetivo)
                return True
            if time.monotonic() > fin:
                return False
            self.drain(0.1)

    def send(self, datos, secreto=False):
        if isinstance(datos, str):
            datos = datos.encode()
        if self.espejo:
            if secreto:
                print("  > ********", flush=True)
            else:
                visible = (datos.decode(errors="replace")
                           .replace("\x03", "<Ctrl+C>")
                           .replace("\x04", "<Ctrl+D>")
                           .replace("\x1a", "<Ctrl+Z>"))
                for linea in visible.split("\n"):
                    if linea.strip():
                        print(f"  > {linea}", flush=True)
        os.write(self.fd, datos)

    def texto(self, desde=0):
        return self.buf[desde:].decode(errors="replace")

    def marca(self):
        return len(self.buf)

    def esperar_salida(self, timeout=15):
        fin = time.monotonic() + timeout
        while time.monotonic() < fin:
            self.drain(0.05)
            pid, st = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                self.status = st
                self._volcar_resto()
                return os.waitstatus_to_exitcode(st)
        os.kill(self.pid, signal.SIGKILL)
        _, st = os.waitpid(self.pid, 0)
        self.status = st
        self._volcar_resto()
        return None

    def echo(self):
        try:
            lflag = termios.tcgetattr(self.fd)[3]
        except OSError:
            return None
        return bool(lflag & termios.ECHO)

    def cerrar(self):
        self._volcar_resto()
        try:
            os.close(self.fd)
        except OSError:
            pass


def login(s, usuario, clave):
    s.wait_for("Usuario: ")
    s.send(f"{usuario}\n")
    s.wait_for("Contrase")
    s.send(f"{clave}\n", secreto=True)


def login_ok(s, usuario, clave):
    login(s, usuario, clave)
    return s.wait_for("Opción:")


def logout(s):
    s.send("16\n")
    return s.wait_for("Usuario: ")


def salir_por_eof(s):
    s.send(b"\x04")
    return s.esperar_salida()


def superbloque_limpio(datos):
    return struct.unpack_from("<I", (datos / "minios.disk").read_bytes(), 20)[0]


def leer_log(datos):
    ruta = datos / "bitacora.log"
    if not ruta.exists():
        return ""
    return ruta.read_text(encoding="utf-8", errors="replace")


# --------------------------------------------------------------------------
# Etapas
# --------------------------------------------------------------------------

def stage_build():
    seccion("Compilación estricta y artefactos")
    traza(f"make clean   (cwd={PROJECT})")
    subprocess.run(["make", "clean"], cwd=PROJECT, capture_output=True, text=True)
    captura("limpieza de artefactos")
    estricto = ("-std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic "
                "-Werror -Iinclude -pthread")
    traza(f'make CFLAGS="{estricto}"')
    r = subprocess.run(["make", f"CFLAGS={estricto}"], cwd=PROJECT,
                       capture_output=True, text=True)
    check("compila con -Werror sin advertencias", r.returncode == 0, r.stderr)
    check("binario ./minios generado", BIN.exists())
    check("modo del binario ejecutable", BIN.exists() and os.access(BIN, os.X_OK))
    captura("compilacion estricta con -Werror")

    ROOT.mkdir(parents=True, exist_ok=True)
    shim = ROOT / "sav_race_shim.so"
    traza(f"gcc -shared -fPIC -O2 -o {shim} {SHIM_SRC} -ldl")
    rc = subprocess.run(["gcc", "-shared", "-fPIC", "-O2", "-o", str(shim),
                         str(SHIM_SRC), "-ldl"], capture_output=True, text=True)
    check("shim de prueba compilado", rc.returncode == 0, rc.stderr)
    captura("compilacion del shim de concurrencia")

    seccion("Archivos fuente")
    fuentes = sorted((PROJECT / "src").glob("*.c"))
    cabeceras = sorted((PROJECT / "include").glob("*.h"))
    check("10 fuentes .c", len(fuentes) == 10, [p.name for p in fuentes])
    check("9 cabeceras .h", len(cabeceras) == 9, [p.name for p in cabeceras])
    check("terminal.c incluido en el build", any(p.name == "terminal.c" for p in fuentes))
    captura("revision de fuentes y cabeceras")


def stage_base():
    seccion("Arranque, RAID, usuarios y permisos")
    w = sandbox("base")
    s = Mini(w)
    check("login admin", login_ok(s, "admin", "admin123"))
    s.drain(0.5)
    t = s.texto()
    check("primer arranque crea el arreglo RAID", "arreglo nuevo" in t)
    check("crea usuarios iniciales", "Usuarios iniciales creados" in t)
    captura("arranque raid y usuarios iniciales")

    s.send("5\n")
    s.wait_for("Estado:")
    s.drain(0.3)
    t = s.texto()
    check("estado del disco consultable", "Limpio" in t or "En uso (sucio)" in t)
    captura("estado del disco raid")

    s.send("13\n")
    s.wait_for("INFORMACIÓN DE VIRTUALIZACIÓN")
    s.drain(0.5)
    t = s.texto()
    check("módulo virtualización responde", "Firma del hipervisor:" in t)
    check("virtualización clasifica el entorno", "Conclusión:" in t)
    captura("informacion de virtualizacion")

    s.send("14\n")
    s.wait_for("ACERCA DEL SISTEMA")
    s.drain(0.3)
    check("módulo sistema responde", "MiniOS" in s.texto())
    captura("informacion del sistema")

    logout(s)
    check("salida por EOF", salir_por_eof(s) == 0)

    datos = w / "datos"
    check("datos/ con permisos 0700",
          stat.S_IMODE(datos.stat().st_mode) == 0o700)
    for nombre in ("usuarios.dat", "matriz.dat", "minios.disk",
                   "minios_espejo.disk", "bitacora.log"):
        ruta = datos / nombre
        check(f"{nombre} con permisos 0600",
              ruta.exists() and stat.S_IMODE(ruta.stat().st_mode) == 0o600,
              "no existe" if not ruta.exists() else oct(ruta.stat().st_mode))
    check("superbloque marcado limpio al salir", superbloque_limpio(datos) == 1)
    check("bitácora registra el arranque", "ARRANQUE" in leer_log(datos))
    check("bitácora registra el login", "LOGIN EXITOSO" in leer_log(datos))
    captura("permisos y superbloque limpio")


def stage_login():
    seccion("Login, bloqueo persistente y desbloqueo")
    w = sandbox("login")
    s = Mini(w)

    for intento in range(3):
        login(s, "invitado", "clave-mala")
        check(f"fallo {intento + 1}/3 muestra error genérico",
              s.wait_for("incorrectos", 5))
    captura("tres intentos fallidos de invitado")

    login_ok(s, "admin", "admin123")
    s.send("11\n")
    s.wait_for("GESTIÓN DE USUARIOS")
    s.send("1\n")
    s.wait_for("USUARIO")
    s.drain(0.3)
    check("invitado aparece BLOQUEADO tras 3 fallos",
          re.search(r"invitado\s+invitado\s+3\s+BLOQUEADO", s.texto()) is not None)
    captura("invitado bloqueado en la tabla de usuarios")

    s.send("11\n")
    s.wait_for("GESTIÓN DE USUARIOS")
    s.send("3\n")
    s.wait_for("desbloquear")
    s.send("invitado\n")
    check("admin desbloquea la cuenta", s.wait_for("Usuario desbloqueado"))
    captura("desbloqueo de invitado por admin")

    logout(s)
    check("invitado puede entrar tras desbloqueo", login_ok(s, "invitado", "inv123"))
    check("sesión de invitado en el menú", "invitado (invitado)" in s.texto())
    captura("invitado entra tras el desbloqueo")
    logout(s)
    check("salida limpia", salir_por_eof(s) == 0)

    log = leer_log(w / "datos")
    check("bitácora registra los 3 fallos", log.count("LOGIN FALLIDO") +
          log.count("BLOQUEO") >= 3,
          f"fallidos={log.count('LOGIN FALLIDO')} bloqueo={log.count('BLOQUEO')}")
    check("bitácora registra el desbloqueo", "DESBLOQUEAR_USUARIO" in log)


def stage_rbac():
    seccion("RBAC: exclusividad del admin y alerta de denegaciones")
    w = sandbox("rbac")
    s = Mini(w)
    login_ok(s, "admin", "admin123")

    rechazos = 0
    nombres_objeto = {7: "bitacora", 6: "usuarios", 4: "matriz"}
    for rol, objeto, derechos in ((1, 7, 2), (1, 6, 1), (2, 4, 4)):
        s.send("9\n")
        s.wait_for("Modificar matriz")
        s.send("1\n")
        s.wait_for("Rol (")
        s.send(f"{rol}\n")
        s.wait_for("Objeto (")
        s.send(f"{objeto}\n")
        s.wait_for("Nuevos derechos")
        s.send(f"{derechos}\n")
        if s.wait_for("exclusivo del administrador", 5):
            rechazos += 1
            captura(f"admin rechaza delegar {nombres_objeto[objeto]}")
    check("rechaza delegar bitácora, usuarios y matriz", rechazos == 3,
          f"rechazos={rechazos}")

    matriz = (w / "datos" / "matriz.dat").read_bytes()
    esperada = bytes([
        0x0F, 0x10, 0x0F, 0x1F, 0x06, 0x12, 0x07, 0x02, 0x10,
        0x07, 0x10, 0x07, 0x06, 0x02, 0x02, 0x04, 0x00, 0x10,
        0x02, 0x00, 0x00, 0x02, 0x00, 0x02, 0x04, 0x00, 0x00,
    ])
    check("matriz intacta tras los rechazos", matriz == esperada)
    logout(s)
    salir_por_eof(s)

    s = Mini(w)
    check("login invitado", login_ok(s, "invitado", "inv123"))
    denegaciones = 0
    for _ in range(5):
        s.send("10\n")
        if s.wait_for("ACCESO DENEGADO", 5):
            denegaciones += 1
    check("5 accesos denegados consecutivos", denegaciones == 5,
          f"denegaciones={denegaciones}")
    check("alerta de seguridad a las 5 denegaciones",
          s.wait_for("[ALERTA SEGURIDAD]", 3))
    t = s.texto()
    check("la alerta se emite una sola vez", t.count("[ALERTA SEGURIDAD]") == 1)
    captura("cinco denegaciones y alerta de seguridad")

    s.send("0\n")
    check("invitado no puede apagar MiniOS", s.wait_for("ACCESO DENEGADO", 5))
    captura("invitado no puede apagar MiniOS")
    s.send("11\n")
    check("invitado no puede gestionar usuarios", s.wait_for("ACCESO DENEGADO", 5))
    check("no hay una segunda alerta", s.texto().count("[ALERTA SEGURIDAD]") == 1)
    captura("invitado no puede gestionar usuarios")
    logout(s)
    salir_por_eof(s)

    log = leer_log(w / "datos")
    check("bitácora: bitácora denegada a invitado",
          re.search(r"invitado .* DERECHO_0x02 .* bitacora .* DENEGADO", log) is not None)
    check("bitácora: alerta registrada", "ALERTA_SEGURIDAD" in log)
    check("sin PERMITIDO de bitácora para invitado",
          re.search(r"invitado .* DERECHO_0x02 .* bitacora .* PERMITIDO", log) is None)

    # Un matriz.dat manipulado a mano debe normalizarse al cargar.
    s = Mini(w)
    login_ok(s, "admin", "admin123")
    logout(s)
    salir_por_eof(s)
    matriz = bytearray((w / "datos" / "matriz.dat").read_bytes())
    matriz[16] = 2  # operador + bitácora D_READ
    (w / "datos" / "matriz.dat").write_bytes(matriz)
    s = Mini(w)
    s.wait_for("Usuario: ")
    salir_por_eof(s)
    check("matriz manipulada se normaliza al cargar",
          (w / "datos" / "matriz.dat").read_bytes()[16] == 0)
    captura("matriz manipulada normalizada al cargar")


def stage_crud():
    seccion("CRUD de registros, propiedad y bitácora")
    w = sandbox("crud")
    s = Mini(w)
    login_ok(s, "admin", "admin123")

    s.send("1\n")
    s.wait_for("Nombre: ")
    s.send("DocAdmin\n")
    s.wait_for("Descripción: ")
    s.send("documento del admin\n")
    m = s.marca()
    s.wait_for("creado")
    encontrado = re.search(r"Registro ID (\d+) creado", s.texto(m))
    check("admin crea un registro", encontrado is not None, s.texto(m)[-300:])
    id_admin = encontrado.group(1) if encontrado else "0"
    captura("admin crea un registro")

    # El operador no tiene D_DELETE por defecto; se lo concede el admin para
    # que la negativa venga del control de propiedad y no del menú.
    s.send("9\n")
    s.wait_for("Modificar matriz")
    s.send("1\n")
    s.wait_for("Rol (")
    s.send("1\n")
    s.wait_for("Objeto (")
    s.send("0\n")
    s.wait_for("Nuevos derechos")
    s.send("15\n")
    check("admin delega D_DELETE de registros al operador",
          s.wait_for("Matriz actualizada", 5))
    captura("admin delega el borrado al operador")
    logout(s)
    salir_por_eof(s)

    s = Mini(w)
    login_ok(s, "operador", "oper123")
    s.send("3\n")
    s.wait_for("ID a actualizar")
    s.send(f"{id_admin}\n")
    check("operador no actualiza registro ajeno",
          s.wait_for("no es propietario", 5))
    captura("operador no actualiza registro ajeno")
    s.send("4\n")
    s.wait_for("ID a borrar")
    s.send(f"{id_admin}\n")
    check("operador no borra registro ajeno", s.wait_for("no es propietario", 5))
    captura("operador no borra registro ajeno")

    s.send("1\n")
    s.wait_for("Nombre: ")
    s.send("DocOper\n")
    s.wait_for("Descripción: ")
    s.send("documento del operador\n")
    m = s.marca()
    s.wait_for("creado")
    encontrado = re.search(r"Registro ID (\d+) creado", s.texto(m))
    check("operador crea su registro", encontrado is not None)
    id_oper = encontrado.group(1) if encontrado else "0"
    captura("operador crea su registro")

    s.send("3\n")
    s.wait_for("ID a actualizar")
    s.send(f"{id_oper}\n")
    s.wait_for("Nuevo nombre: ")
    s.send("DocOper2\n")
    s.wait_for("Nueva descripción: ")
    s.send("documento actualizado\n")
    check("operador actualiza su registro", s.wait_for("actualizado", 5))
    captura("operador actualiza su registro")

    s.send("2\n")
    s.wait_for("PROPIETARIO")
    s.drain(0.3)
    t = s.texto()
    check("lista muestra el registro del admin", "DocAdmin" in t)
    check("lista muestra el registro actualizado", "DocOper2" in t)
    captura("operador consulta los registros")

    s.send("4\n")
    s.wait_for("ID a borrar")
    s.send(f"{id_oper}\n")
    check("operador borra su registro", s.wait_for("eliminado", 5))
    captura("operador borra su propio registro")
    logout(s)
    salir_por_eof(s)

    s = Mini(w)
    login_ok(s, "admin", "admin123")
    s.send("4\n")
    s.wait_for("ID a borrar")
    s.send(f"{id_admin}\n")
    check("admin borra un registro ajeno", s.wait_for("eliminado", 5))
    captura("admin borra un registro")
    s.send("2\n")
    s.wait_for("PROPIETARIO")
    s.drain(0.3)
    t = s.texto()
    check("lista vacía tras borrar", "DocAdmin" not in t and "DocOper2" not in t)
    captura("lista vacia tras borrar")

    s.send("10\n")
    s.wait_for("RESUMEN DE AUDITORÍA")
    s.drain(0.3)
    t = s.texto()
    check("bitácora muestra resumen", "Accesos denegados:" in t)
    check("bitácora cuenta logins fallidos", "Intentos de login fallidos:" in t)
    captura("resumen de auditoria")
    logout(s)
    check("salida limpia", salir_por_eof(s) == 0)

    log = leer_log(w / "datos")
    check("bitácora registra actualización denegada",
          re.search(r"ACTUALIZAR_REGISTRO\s*\|\s*propiedad\s*\|\s*DENEGADO", log) is not None)
    check("bitácora registra borrado denegado",
          re.search(r"ELIMINAR_REGISTRO\s*\|\s*propiedad\s*\|\s*DENEGADO", log) is not None)


def stage_signals():
    seccion("Señales: SIGINT/SIGTERM, terminal y disco")
    w = sandbox("signals")

    s = Mini(w)
    s.wait_for("Usuario: ")
    s.send("admin\n")
    s.wait_for("Contrase")
    s.send(b"\x03")
    codigo = s.esperar_salida()
    check("Ctrl+C en contraseña sale con 130", codigo == 130, f"codigo={codigo}")
    check("ECHO restaurado tras Ctrl+C", s.echo() is True)
    check("superbloque limpio tras Ctrl+C", superbloque_limpio(w / "datos") == 1)
    captura("ctrl-c en el login restaura la terminal")
    s.cerrar()

    s = Mini(w)
    s.wait_for("Usuario: ")
    s.drain(0.3)
    check("segundo arranque sin aviso de apagado sucio",
          "apagado sucio" not in s.texto())
    check("salida por EOF en login", salir_por_eof(s) == 0)
    captura("segundo arranque sin apagado sucio")
    s.cerrar()

    s = Mini(w)
    login_ok(s, "admin", "admin123")
    s.send(b"\x03")
    codigo = s.esperar_salida()
    check("Ctrl+C en el menú sale con 130", codigo == 130, f"codigo={codigo}")
    check("ECHO intacto en el menú", s.echo() is True)
    check("disco limpio tras Ctrl+C en el menú",
          superbloque_limpio(w / "datos") == 1)
    captura("ctrl-c en el menu cierra limpio")
    s.cerrar()

    s = Mini(w)
    login_ok(s, "admin", "admin123")
    os.kill(s.pid, signal.SIGTERM)
    codigo = s.esperar_salida()
    check("SIGTERM sale con 143", codigo == 143, f"codigo={codigo}")
    check("disco limpio tras SIGTERM", superbloque_limpio(w / "datos") == 1)
    captura("sigterm cierra limpio")
    s.cerrar()


def stage_game():
    seccion("Snake: autoguardado, pausa, Ctrl+C, corrupción y kill -9")
    w = sandbox("game")
    datos = w / "datos"
    s = Mini(w)
    login_ok(s, "admin", "admin123")

    s.send("7\n")
    check("inicia Snake", s.wait_for("MINI SNAKE", 5))
    s.drain(17)
    check("autoguardado a los 15 s", "Autoguardado realizado" in s.texto())
    s.send("q")
    check("q pausa y guarda", s.wait_for("Partida pausada y guardada", 10))
    s.wait_for("Opción:")

    sav = datos / "partida_admin.sav"
    check(".sav existe", sav.exists())
    check(".sav mide 1680 bytes", sav.exists() and sav.stat().st_size == 1680)
    check(".sav con permisos 0600",
          sav.exists() and stat.S_IMODE(sav.stat().st_mode) == 0o600)
    captura("snake autoguardado y pausa")

    s.send("8\n")
    check("continuar partida", s.wait_for("Partida recuperada", 10))
    s.send("q")
    s.wait_for("Opción:", 10)
    captura("snake continua la partida guardada")

    s.send("7\n")
    check("nueva partida", s.wait_for("MINI SNAKE", 5))
    s.drain(1)
    s.send(b"\x03")
    check("Ctrl+C dentro del juego pausa y guarda",
          s.wait_for("Partida pausada y guardada", 10))
    check("terminal restaurado tras Ctrl+C en juego", s.echo() is True)
    s.wait_for("Opción:")
    captura("ctrl-c dentro del juego pausa y guarda")

    sav.write_bytes(b"basura")
    s.send("8\n")
    check("rechaza un .sav corrupto",
          s.wait_for("no es válida", 5))
    captura("snake rechaza un archivo corrupto")

    s.send("7\n")
    s.wait_for("MINI SNAKE", 5)
    s.send("w")
    check("choque contra el borde produce GAME OVER",
          s.wait_for("GAME OVER", 8))
    s.wait_for("Opción:", 10)
    s.send("8\n")
    check("partida terminada no se puede continuar",
          s.wait_for("no es válida", 5))

    s.send("2\n")
    s.wait_for("PROPIETARIO")
    s.drain(0.3)
    check("récord del juego guardado en el disco virtual",
          "Record Snake - admin" in s.texto())
    captura("snake game over y record guardado")

    m = s.marca()
    s.send("7\n")
    s.wait_for("MINI SNAKE", 5)
    s.drain(16)
    check("autoguardado antes de kill -9",
          "Autoguardado realizado" in s.texto(m))
    os.kill(s.pid, signal.SIGKILL)
    s.esperar_salida()
    s.cerrar()

    s = Mini(w)
    s.wait_for("Usuario: ")
    s.drain(0.3)
    check("kill -9 deja aviso de apagado sucio",
          "apagado sucio" in s.texto())
    login_ok(s, "admin", "admin123")
    s.send("8\n")
    check("recupera la partida tras kill -9",
          s.wait_for("Partida recuperada", 10))
    s.send("q")
    s.wait_for("Opción:", 10)
    captura("snake se recupera tras kill -9")
    logout(s)
    check("salida limpia", salir_por_eof(s) == 0)


def stage_raid():
    seccion("RAID 1: reconstrucción desde el espejo")
    w = sandbox("raid")
    s = Mini(w)
    login_ok(s, "admin", "admin123")
    s.send("1\n")
    s.wait_for("Nombre: ")
    s.send("DocRAID\n")
    s.wait_for("Descripción: ")
    s.send("sobrevive al borrado del disco principal\n")
    check("registro creado antes del fallo", s.wait_for("creado", 5))
    captura("registro creado antes del fallo")
    s.send("0\n")
    check("apagado normal", s.wait_for("Apagando", 5))
    check("salida 0", s.esperar_salida() == 0)
    s.cerrar()

    principal = w / "datos" / "minios.disk"
    check("existe el disco principal", principal.exists())
    principal.unlink()
    check("disco principal borrado", not principal.exists())

    s = Mini(w)
    s.wait_for("Usuario: ")
    s.drain(0.5)
    t = s.texto()
    check("detecta disco principal ausente", "Reconstruyendo desde el espejo" in t)
    check("reconstruye el disco principal", "Disco principal reconstruido" in t)
    login_ok(s, "admin", "admin123")
    s.send("2\n")
    s.wait_for("PROPIETARIO")
    s.drain(0.3)
    check("los registros sobreviven a la reconstrucción",
          "DocRAID" in s.texto())
    logout(s)
    salir_por_eof(s)
    check("el disco reconstruido existe", principal.exists())
    captura("raid reconstruido desde el espejo")


def stage_proc():
    seccion("Procesos: lanzar, suspender, memoria, reanudar y terminar")
    w = sandbox("proc")
    bin_dir = w / "bin"
    bin_dir.mkdir()
    falso = bin_dir / "tetris-bsd"
    fuente = bin_dir / "falso.c"
    fuente.write_text("#include <unistd.h>\nint main(void) { for (;;) pause(); }\n")
    traza(f"gcc -O2 -o {falso} {fuente}   # tetris-bsd de prueba")
    compilado = subprocess.run(["gcc", "-O2", "-o", str(falso), str(fuente)],
                               capture_output=True, text=True)
    check("binario de prueba compilado", compilado.returncode == 0, compilado.stderr)

    s = Mini(w, path_extra=str(bin_dir))
    login_ok(s, "admin", "admin123")
    s.send("6\n")
    s.wait_for("GESTIÓN DE PROCESOS")
    s.send("1\n")
    m = s.marca()
    check("lanza proceso hijo", s.wait_for("iniciado con PID", 5))
    encontrado = re.search(r"PID (\d+)", s.texto(m))
    pid = encontrado.group(1) if encontrado else "0"
    time.sleep(0.4)
    # El PTY del arnés deja el grupo huérfano (MiniOS es líder de sesión),
    # así que el kernel descarta SIGTSTP (regla POSIX de grupos huérfanos).
    # SIGSTOP detiene el hijo igual y ejercita el mismo camino: waitpid con
    # WUNTRACED -> SUSPENDIDO -> SIGCONT al reanudar.
    os.kill(int(pid), signal.SIGSTOP)
    check("parada del hijo detectada (WUNTRACED)", s.wait_for("suspendido", 8))
    captura("proceso lanzado y suspendido")

    s.send("15\n")
    check("monitor de memoria disponible", s.wait_for("VmSize:", 5))
    s.drain(0.3)
    t = s.texto()
    check("monitor reporta VmRSS", "VmRSS:" in t)
    check("monitor reporta páginas", "páginas" in t)
    captura("monitor de memoria del proceso")

    s.send("6\n")
    s.wait_for("GESTIÓN DE PROCESOS")
    s.send("3\n")
    s.wait_for("PID a reanudar")
    s.send(f"{pid}\n")
    time.sleep(0.5)
    os.kill(int(pid), signal.SIGSTOP)
    check("reanuda y vuelve a suspender", s.wait_for("SUSPENDIDO", 8))
    captura("proceso reanudado y suspendido de nuevo")

    s.send("6\n")
    s.wait_for("GESTIÓN DE PROCESOS")
    s.send("4\n")
    s.wait_for("PID a terminar")
    s.send(f"{pid}\n")
    check("termina y recolecta el proceso", s.wait_for("terminado y recolectado", 8))
    captura("proceso terminado y recolectado")

    # Propiedad: el operador no puede terminar un proceso del admin.
    s.send("6\n")
    s.wait_for("GESTIÓN DE PROCESOS")
    s.send("1\n")
    m = s.marca()
    s.wait_for("iniciado con PID", 5)
    encontrado = re.search(r"PID (\d+)", s.texto(m))
    pid2 = encontrado.group(1) if encontrado else "0"
    time.sleep(0.4)
    os.kill(int(pid2), signal.SIGSTOP)
    s.wait_for("suspendido", 8)
    logout(s)

    check("login operador", login_ok(s, "operador", "oper123"))
    s.send("6\n")
    s.wait_for("GESTIÓN DE PROCESOS")
    s.send("4\n")
    s.wait_for("PID a terminar")
    s.send(f"{pid2}\n")
    check("operador no termina proceso ajeno", s.wait_for("no es propietario", 5))
    captura("operador no termina proceso ajeno")
    logout(s)

    check("login admin de nuevo", login_ok(s, "admin", "admin123"))
    s.send("6\n")
    s.wait_for("GESTIÓN DE PROCESOS")
    s.send("4\n")
    s.wait_for("PID a terminar")
    s.send(f"{pid2}\n")
    check("admin sí termina el proceso", s.wait_for("terminado y recolectado", 8))

    s.send("6\n")
    s.wait_for("GESTIÓN DE PROCESOS")
    s.send("2\n")
    s.wait_for("PROGRAMA")
    s.drain(0.3)
    check("tabla conserva el estado TERMINADO", "TERMINADO" in s.texto())
    captura("tabla de procesos con estado terminado")
    logout(s)
    check("salida limpia", salir_por_eof(s) == 0)


def stage_race():
    seccion("Carrera de guardado con shim LD_PRELOAD")
    w = sandbox("race")
    s = Mini(w, env_extra={"LD_PRELOAD": str(ROOT / "sav_race_shim.so")})
    check("login admin", login_ok(s, "admin", "admin123"))
    s.send("7\n")
    check("inicia Snake", s.wait_for("MINI SNAKE", 5))
    t0 = time.monotonic()
    s.drain(14.8)
    s.send("g")
    while time.monotonic() < t0 + 15.1:
        s.drain(0.05)
    s.send("g")
    s.drain(2.5)
    s.send("q")
    s.wait_for("Partida pausada", 10)
    s.drain(0.5)
    t = s.texto()
    renames = re.findall(r"rename\([^)]*\) = (-?\d+)", t)
    check("el shim registra escrituras", len(renames) >= 2, f"renames={len(renames)}")
    check("todos los rename tienen éxito",
          bool(renames) and all(int(r) == 0 for r in renames), renames)
    check("ningún rename devolvió error", "= -1" not in t)
    check("sin aviso de autoguardado fallido", "No se pudo autoguardar" not in t)
    check("sin aviso de guardado manual fallido",
          "No se pudo guardar la partida" not in t)
    check("los dos guardados usan hilos distintos",
          len(set(re.findall(r"tid=(\d+)", t))) >= 2,
          set(re.findall(r"tid=(\d+)", t)))
    sav = w / "datos" / "partida_admin.sav"
    check(".sav final válido", sav.exists() and sav.stat().st_size == 1680)
    captura("carrera de guardado sin errores")
    logout(s)
    check("salida limpia", salir_por_eof(s) == 0)


def stage_edge():
    seccion("Entradas inválidas, EOF y límites")
    w = sandbox("edge")
    s = Mini(w)

    # A) Línea más larga que el buffer: no debe romper el proceso. El resto
    #    queda como contraseña (defecto menor conocido: no se drena la entrada
    #    sobrante), por eso esta sesión se cierra aquí y no se sigue usando.
    s = Mini(w)
    s.wait_for("Usuario: ")
    s.send("x" * 60 + "\n")
    s.wait_for("Contrase")
    s.send("y" * 100 + "\n")
    check("usuario/clave largos no rompen el login",
          s.wait_for("incorrectos", 5))
    os.kill(s.pid, signal.SIGTERM)
    check("sesión con entrada larga se cierra limpia",
          s.esperar_salida() == 143)
    captura("entrada larga no rompe el login")
    s.cerrar()

    # B) Flujo normal con entradas inválidas.
    s = Mini(w)
    check("login admin", login_ok(s, "admin", "admin123"))
    s.send("abc\n")
    check("menú no numérico muestra opción inválida",
          s.wait_for("Opción inválida", 5))
    captura("menu con texto no numerico")
    s.send("999\n")
    check("opción fuera de rango muestra opción inválida",
          s.wait_for("Opción inválida", 5))
    captura("opcion de menu fuera de rango")

    s.send("1\n")
    s.wait_for("Nombre: ")
    s.send("\n")
    check("registro con nombre vacío se cancela",
          s.wait_for("Nombre vacío", 5))
    captura("nombre vacio cancela el registro")

    s.send("1\n")
    s.wait_for("Nombre: ")
    s.send("N" * 48 + "\n")
    s.wait_for("Descripción: ")
    s.send("D" * 98 + "\n")
    check("nombre/descripción en el límite no rompen",
          s.wait_for("creado", 5))
    captura("nombre y descripcion en el limite")

    s.send("12\n")
    s.wait_for("Contraseña actual")
    s.send("incorrecta\n")
    check("cambio de clave con actual incorrecta",
          s.wait_for("actual incorrecta", 5))
    captura("cambio de clave con actual incorrecta")

    s.send(b"\x04")
    codigo = s.esperar_salida()
    check("EOF en el menú sale con 0", codigo == 0, f"codigo={codigo}")
    check("EOF en el menú se audita", "EOF_EN_MENU" in leer_log(w / "datos"))
    captura("eof en el menu")
    s.cerrar()

    s = Mini(w)
    s.wait_for("Usuario: ")
    check("EOF en login sale con 0", salir_por_eof(s) == 0)
    check("EOF en login se audita", "EOF_EN_LOGIN" in leer_log(w / "datos"))
    captura("eof en el login")
    s.cerrar()


ETAPAS = {
    "build": stage_build,
    "base": stage_base,
    "login": stage_login,
    "rbac": stage_rbac,
    "crud": stage_crud,
    "signals": stage_signals,
    "game": stage_game,
    "raid": stage_raid,
    "proc": stage_proc,
    "race": stage_race,
    "edge": stage_edge,
}

ORDEN = ["build", "base", "login", "rbac", "crud", "signals",
         "game", "raid", "proc", "race", "edge"]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage", default="all", choices=["all"] + ORDEN)
    args = parser.parse_args()

    print(f"MiniOS v3.0 - suite de pruebas | proyecto: {PROJECT}", flush=True)
    print(f"fecha: {time.strftime('%Y-%m-%d %H:%M:%S')}", flush=True)

    etapas = ORDEN if args.stage == "all" else [args.stage]
    for nombre in etapas:
        print(f"\n{'#' * 70}\n# ETAPA: {nombre}\n{'#' * 70}", flush=True)
        try:
            ETAPAS[nombre]()
        except Exception as exc:  # una etapa rota no debe ocultar el resto
            check(f"etapa {nombre} sin excepciones", False, repr(exc))

    print(f"\n{'=' * 70}")
    print(f"RESULTADO: {pasos - len(fallos)}/{pasos} comprobaciones correctas")
    if fallos:
        print("FALLOS:")
        for nombre in fallos:
            print(f"  - {nombre}")
    print("=" * 70, flush=True)
    return 1 if fallos else 0


if __name__ == "__main__":
    sys.exit(main())
