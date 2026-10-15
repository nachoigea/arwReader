# arw_colorspace.py — arwReader
# ---------------------------------------------------------------------------
# El plugin entrega los píxeles ya convertidos al espacio lineal elegido en el
# knob "output space" (arw_space). El knob "colorspace" del Read debe decir
# lo MISMO para que Nuke no aplique una segunda conversión.
#
# Este callback lo hace automáticamente cuando cambia el archivo o arw_space:
# busca, entre los colorspaces que ofrece la configuración OCIO activa
# (nuke-default, ACES 1.x, ACES cg-config/studio-config...), el primero que
# coincida con el espacio de salida. Si no encuentra ninguno no toca nada.
# ---------------------------------------------------------------------------
import nuke

RAW_EXTENSIONS = (".arw", ".sr2", ".srf")

# Índice de arw_space -> nombres candidatos, en orden de preferencia.
# Debe seguir el orden de kSpaceNames en src/arwReader.cpp.
CANDIDATES = {
    0: ["linear", "Linear Rec.709 (sRGB)", "lin_rec709", "Utility - Linear - sRGB", "lin_srgb"],
    1: ["ACEScg", "ACES - ACEScg"],
    2: ["ACES2065-1", "ACES - ACES2065-1"],
    3: ["Linear Rec.2020", "lin_rec2020", "Utility - Linear - Rec.2020"],
    4: ["Linear CIE-XYZ D65", "CIE-XYZ-D65", "lin_ciexyz_d65"],
    5: ["raw", "Raw", "Utility - Raw"],
}

WATCHED_KNOBS = ("file", "arw_space", "showPanel")


def _is_raw(node):
    try:
        return node["file"].value().lower().endswith(RAW_EXTENSIONS)
    except Exception:
        return False


def _find_colorspace(knob, wanted):
    """Devuelve el valor exacto del menú que coincide con alguno de 'wanted'.

    Los valores pueden venir con familia ("Utility/Utility - Linear - sRGB")
    y, en algunas versiones, con el rol delante ("default (linear)"),
    así que se compara solo la última parte y sin mayúsculas."""
    options = {}
    for value in knob.values():
        leaf = value.split("/")[-1].strip()
        options.setdefault(leaf.lower(), value)
    for name in wanted:
        if name.lower() in options:
            return options[name.lower()]
    return None


def sync(node=None):
    """Ajusta el colorspace de un Read de RAW al espacio de salida del plugin."""
    node = node or nuke.thisNode()
    if node is None or node.Class() != "Read" or not _is_raw(node):
        return
    space_knob = node.knob("arw_space")
    cs_knob = node.knob("colorspace")
    if space_knob is None or cs_knob is None:
        return  # el plugin aún no ha creado sus knobs
    match = _find_colorspace(cs_knob, CANDIDATES.get(int(space_knob.getValue()), []))
    if match and cs_knob.value() != match:
        cs_knob.setValue(match)


def _on_knob_changed():
    knob = nuke.thisKnob()
    if knob is not None and knob.name() in WATCHED_KNOBS:
        sync(nuke.thisNode())


def register():
    nuke.addKnobChanged(_on_knob_changed, nodeClass="Read")
