# init.py — arwReader
# ---------------------------------------------------------------------------
# Nuke ejecuta este archivo al arrancar si su carpeta está en el plugin path
# (por ejemplo, con  nuke.pluginAddPath("arwReader")  en ~/.nuke/init.py).
#
# Cada versión mayor.menor de Nuke necesita su propio binario compilado con su
# NDK, así que se guardan en subcarpetas "14.0", "15.1", "16.0"... y aquí se
# añade al plugin path SOLO la que corresponde a la versión que se está
# ejecutando. Así un mismo directorio sirve para varias versiones instaladas.
# ---------------------------------------------------------------------------
import os
import nuke

_here = os.path.dirname(os.path.abspath(__file__))
_version_dir = os.path.join(_here, "{}.{}".format(nuke.NUKE_VERSION_MAJOR, nuke.NUKE_VERSION_MINOR))

if os.path.isdir(_version_dir):
    nuke.pluginAddPath(_version_dir)
else:
    nuke.tprint(
        "[arwReader] No hay binario para Nuke {}.{} en {} — compílalo con "
        "-DNUKE_ROOT apuntando a esta versión.".format(
            nuke.NUKE_VERSION_MAJOR, nuke.NUKE_VERSION_MINOR, _here
        )
    )

# Sincronización opcional del colorspace del Read (ver arw_colorspace.py).
# Desactivar con la variable de entorno ARW_AUTO_COLORSPACE=0.
if os.environ.get("ARW_AUTO_COLORSPACE", "1") != "0":
    try:
        import arw_colorspace
        arw_colorspace.register()
    except Exception as exc:  # nunca bloquear el arranque de Nuke
        nuke.tprint("[arwReader] arw_colorspace desactivado: {}".format(exc))
