# arwReader — Sony RAW (.ARW) nativo en Nuke

Plugin de lectura para **The Foundry Nuke 14 y posteriores** que abre archivos RAW de cámaras Sony (`.arw`, y también `.sr2` / `.srf`) directamente en el nodo **Read**. Los píxeles llegan al árbol como **float 32 bits lineal, sin recortar**, en el espacio de color que elijas. Eso significa que:

- se conserva todo el rango del sensor (12 o 14 bits), incluidos los valores por encima de 1.0;
- los nodos nativos (Grade, ColorCorrect, HueCorrect…) trabajan sobre los datos completos;
- el **Write** nativo exporta de forma normal a EXR, DPX, TIFF, etc.

---

## Índice

1. [Decisiones de diseño y por qué](#1-decisiones-de-diseño-y-por-qué)
2. [Estructura del proyecto](#2-estructura-del-proyecto)
3. [Flujo de datos](#3-flujo-de-datos)
4. [Documentación de cada parte](#4-documentación-de-cada-parte)
5. [Compilación e instalación](#5-compilación-e-instalación)
6. [Uso en Nuke](#6-uso-en-nuke)
7. [Metadatos](#7-metadatos)
8. [Verificación realizada](#8-verificación-realizada)
9. [Limitaciones conocidas](#9-limitaciones-conocidas)
10. [Licencias](#10-licencias)

---

## 1. Decisiones de diseño y por qué

| Decisión | Alternativas | Motivo |
|---|---|---|
| **Reader nativo en C++ (NDK)** | Gizmo Python + rawpy que genere un EXR intermedio | El `.arw` se abre en el Read igual que un EXR. Funcionan secuencias, proxy, caché y el Write sin pasos intermedios ni archivos temporales. A cambio, hay que compilar un binario por versión de Nuke. |
| **LibRaw** como decodificador | dcraw, SDK de Sony, RawSpeed | Es la referencia libre más mantenida (la usan RawTherapee, darktable y OpenImageIO). Soporta los ARW antiguos y modernos, incluida la compresión *lossless* reciente de Sony, y se actualiza con cada cámara nueva. |
| **LibRaw enlazada estáticamente** dentro del plugin | DLL aparte | Un solo archivo que copiar y sin conflictos con otras copias de LibRaw que pueda cargar Nuke u otro plugin. En Linux, además, no se exportan sus símbolos (`--exclude-libs`). |
| **Salida float 32 lineal sin clamp** | 16 bits enteros | El RAW es lineal por naturaleza, y un float de 32 bits representa exactamente cualquier valor de 12 o 14 bits. Así sobra rango para que el Grade suba o baje exposición sin pérdidas. |
| **Balance de blancos en modo "unclip"** | Clip (lo habitual en reveladores) | Tras el balance, el canal rojo o azul puede superar el blanco del sensor. Un revelador típico lo recorta. Aquí se conserva (valores > 1.0) para que decidas en Nuke qué hacer con las altas luces. |
| **La matriz de color se aplica en float** y no dentro de LibRaw | `output_color` de LibRaw | LibRaw convierte y **recorta** a 16 bits en el mismo paso. Al pedirle RGB de cámara y aplicar la matriz después, en float, se conservan los colores fuera de gama (valores negativos) y las altas luces. |
| **Espacio de salida seleccionable** (Rec.709 lineal, ACEScg, ACES2065-1, Rec.2020 lineal, XYZ, cámara nativa) | Uno fijo | Así encaja tanto en el flujo por defecto de Nuke (`linear`) como en proyectos OCIO/ACES. |
| **Núcleo separado de Nuke** (`RawDecoder`) | Todo en un único archivo | El revelado se puede compilar y probar sin Nuke (`arw_probe`). Si en el futuro se quiere otra integración (OFX, Python), el núcleo se reutiliza tal cual. |
| **Revelado bajo demanda** (cabecera en el constructor, píxeles en el primer `engine`) | Revelar todo al crear el Read | Nuke crea Readers para consultar tamaño y metadatos sin necesitar píxeles. Revelar solo cuando hacen falta ahorra segundos por frame en esas consultas. |

---

## 2. Estructura del proyecto

```
arwReader/
├── CMakeLists.txt            Compilación multiplataforma y multiversión
├── build_windows.bat         Atajo de compilación + instalación en Windows
├── src/
│   ├── RawDecoder.h/.cpp     Núcleo de revelado (sin dependencias de Nuke)
│   └── arwReader.cpp         Reader de Nuke (knobs, metadatos, filas)
├── nuke/
│   ├── init.py               Carga el binario de la versión de Nuke en uso
│   ├── arw_colorspace.py     Sincroniza el colorspace del Read (opcional)
│   ├── sr2Reader.tcl         Alias para .sr2
│   └── srfReader.tcl         Alias para .srf
├── tools/
│   └── arw_probe.cpp         Herramienta de línea de comandos para probar el núcleo
├── docs/
│   └── matrices.py           Cálculo de las matrices de color
└── examples/
    └── arw_grade_example.nk  Read ARW → Grade → ColorCorrect → Write EXR
```

---

## 3. Flujo de datos

```
 archivo .ARW
     │
     ▼
 LibRaw open_file + unpack ............ mosaico Bayer 12/14 bits + metadatos
     │
     ▼
 LibRaw dcraw_process
   · resta negro, normaliza a blanco del sensor
   · balance de blancos ÷ multiplicador MAYOR   ← nada desborda 16 bits
   · demosaicing (AHD por defecto)
   · salida: RGB de cámara, lineal, 16 bits
     │
     ▼
 RawDecoder (float 32)
   · ÷ 65535 · × (1 / multiplicador menor)       ← recupera el rango > 1.0
   · × 2^exposición
   · matriz cámara → Rec.709 → espacio de salida (una sola 3×3 combinada)
   · recorte de cámara + orientación
     │
     ▼
 ArwReader::engine (Nuke)
   · invierte filas (Nuke cuenta desde abajo)
   · from_float → colorspace del Read
     │
     ▼
 Grade / ColorCorrect / … → Write
```

---

## 4. Documentación de cada parte

### 4.1 `src/RawDecoder.h / .cpp` — núcleo de revelado

Es independiente de Nuke y expone dos funciones estáticas:

- **`readInfo(path, options, info, error)`**: solo lee la cabecera, en milisegundos. Devuelve el tamaño final (ya con recorte, *half size* y orientación aplicados), el *pixel aspect*, la profundidad de bits real y los metadatos.
- **`decode(path, options, image, info, error)`**: revelado completo a `Image` (RGB float entrelazado, fila 0 = arriba).

**`DecodeOptions`** (todas las opciones del revelado):

| Campo | Valores | Efecto |
|---|---|---|
| `outputSpace` | `LinearRec709`, `ACEScg`, `ACES2065_1`, `LinearRec2020`, `XYZ_D65`, `CameraNative` | Espacio lineal de salida |
| `whiteBalance` | `AsShot`, `Auto`, `Daylight`, `Custom` | Origen de los multiplicadores de balance |
| `customMul[3]` | floats > 0 | Multiplicadores R, G, B para `Custom` |
| `highlights` | `Unclip`, `Clip`, `Blend`, `Reconstruct` | Tratamiento de canales saturados |
| `demosaic` | `AHD`, `AAHD`, `DHT`, `DCB`, `PPG`, `VNG`, `Linear` | Algoritmo de interpolación |
| `exposureStops` | float | Multiplica por 2^stops (lineal, sin recorte) |
| `halfSize` | bool | Media resolución sin demosaicing (proxy rápido) |
| `applyCrop` | bool | Aplica el recorte útil definido por la cámara |

**Pasos internos de `decode()`:**

1. **`configure()`** prepara LibRaw para entregar datos lineales (`gamm = 1,1`), sin auto-brillo, en 16 bits y en RGB de cámara (`output_color = 0`). La orientación la gestiona el plugin (`user_flip = 0`).
2. **Normalización del balance de blancos.** Con `highlight ≥ 1`, LibRaw divide los multiplicadores entre el **mayor**, así que ningún canal supera 65535. El plugin multiplica después por `1 / min(pre_mul)` para que el canal con el multiplicador menor (normalmente el verde) vuelva a tener su blanco de sensor en 1.0. Los otros canales pueden pasar de 1.0: es la información que un revelado normal tiraría.
3. **Matriz de color.** `rgb_cam` de LibRaw convierte el RGB de cámara (ya balanceado) a Rec.709/sRGB lineal, y cada fila suma 1, de modo que el blanco se mantiene. Se multiplica por la matriz Rec.709 → espacio de salida y por la escala del paso 2, y se aplica como **una única 3×3 por píxel**.
4. **Recorte y orientación.** Se usa el recorte *inset* que guarda la cámara. Muchas Sony dan el tamaño pero no la posición (`0xFFFF`), y en ese caso se centra. La orientación EXIF (180°, 90° horario y antihorario) se aplica reordenando los índices al copiar, sin pasos extra.

**Matrices de color**: las calcula `docs/matrices.py` a partir de los primarios oficiales. Los espacios ACES usan adaptación cromática Bradford D65 → blanco ACES (≈D60). Coinciden con las de OCIO/ACES.

**Rutas Unicode en Windows**: la ruta UTF-8 de Nuke se convierte a `wchar_t` para que funcionen carpetas con tildes o "ñ".

### 4.2 `src/arwReader.cpp` — integración con Nuke

#### `ArwReaderFormat` — knobs del nodo Read

Aparecen en el Read en cuanto el archivo es un `.arw`. Todos llevan el prefijo `arw_`, lo que permite usarlos desde Python (`nuke.toNode('Read1')['arw_exposure'].setValue(1)`).

| Knob | Etiqueta | Descripción |
|---|---|---|
| `arw_space` | output space | Espacio de color lineal de salida |
| `arw_exposure` | exposure (stops) | Exposición previa en pasos |
| `arw_wb` | white balance | as shot / auto / daylight / custom |
| `arw_wb_mul` | custom multipliers | Multiplicadores R G B (con `custom`) |
| `arw_highlights` | highlights | unclip / clip / blend / reconstruct |
| `arw_demosaic` | demosaic | Algoritmo de demosaicing |
| `arw_half_size` | half size (proxy) | Revelado rápido a media resolución |
| `arw_crop` | camera crop | Recorte útil de la cámara |

`append(Hash&)` añade todos los valores al *hash* del Read. Así, cualquier cambio invalida la caché de Nuke y fuerza un nuevo revelado.

#### `ArwReader` — el lector

- **Constructor**: cierra el descriptor que pasa Nuke (LibRaw abre por ruta), lee las opciones del `ArwReaderFormat`, llama a `readInfo()` y fija tamaño, canales RGB y *pixel aspect* con `set_info()`. Además declara los datos como float lineal (`LUT::FLOAT`) y rellena los metadatos.
- **`engine(y, x, r, channels, row)`**: Nuke la llama por fila y desde varios hilos. La primera llamada revela la imagen completa (protegida con un `mutex`). Después, cada llamada solo copia la fila pedida: invierte el eje Y (Nuke numera de abajo arriba) y pasa los datos por `from_float`, que aplica el *colorspace* del Read.
- **`fetchMetaData()`**: devuelve los metadatos (ver §7).
- **Registro**: `Reader::Description("arw\0sr2\0srf\0", …)` con un test de firma TIFF (`II*\0` / `MM\0*`), porque los ARW son contenedores TIFF.

### 4.3 `CMakeLists.txt` — compilación

- Lee la versión de Nuke de `NUKE_ROOT/include/DDImage/ddImageVersionNumbers.h` e **instala en una subcarpeta `<mayor>.<menor>`**. Así un mismo directorio sirve para Nuke 14.0, 15.1, 16.0, etc.
- Descarga **LibRaw** (versión en `ARW_LIBRAW_TAG`, por defecto 0.22.2) y **LibRaw-cmake**, y compila LibRaw estática. También puedes usar una LibRaw ya instalada con `ARW_LIBRAW_ROOT`.
- **Windows**: runtime `/MD` (el mismo que Nuke) y `NOMINMAX`.
- **Linux**: fija `_GLIBCXX_USE_CXX11_ABI` según la versión (0 para Nuke ≤ 14, 1 para Nuke ≥ 15; se puede forzar con `ARW_GLIBCXX_ABI`) y oculta los símbolos de LibRaw.
- El binario se llama `arwReader.dll/.so/.dylib`, **sin prefijo `lib`**, porque Nuke busca exactamente `<extensión>Reader`.

| Opción CMake | Por defecto | Uso |
|---|---|---|
| `NUKE_ROOT` | — | Carpeta de Nuke (obligatoria para el plugin) |
| `ARW_BUILD_PLUGIN` | ON | Compilar el plugin |
| `ARW_BUILD_PROBE` | OFF | Compilar `arw_probe` |
| `ARW_LIBRAW_TAG` | 0.22.2 | Versión de LibRaw a descargar |
| `ARW_LIBRAW_ROOT` | vacío | LibRaw precompilada |
| `ARW_LIBRAW_OPENMP` | OFF | Paralelizar el demosaicing con OpenMP |
| `ARW_GLIBCXX_ABI` | auto | Solo Linux |

### 4.4 `nuke/init.py`

Añade al *plugin path* la subcarpeta del binario que corresponde a la versión de Nuke que se está ejecutando. Si no existe, avisa en la consola de Nuke sin romper el arranque. También registra el callback de `arw_colorspace.py`, salvo que exista `ARW_AUTO_COLORSPACE=0`.

### 4.5 `nuke/arw_colorspace.py`

El plugin ya entrega los píxeles en el espacio elegido en `arw_space`. El *colorspace* del Read debe indicar lo mismo para que Nuke no aplique una segunda conversión. Este callback (`knobChanged` de los Read) busca en la configuración OCIO activa el nombre equivalente y lo selecciona. Ejemplos: `linear` en *nuke-default* y `ACES - ACEScg` en ACES 1.x. Si no encuentra ninguno, no toca nada.

### 4.6 `nuke/sr2Reader.tcl`, `srfReader.tcl`

Nuke busca un plugin llamado `<extensión>Reader`. Estos alias (`load arwReader`) hacen que las extensiones `.sr2` y `.srf`, de cámaras Sony antiguas, usen el mismo binario.

### 4.7 `tools/arw_probe.cpp`

Herramienta de consola para probar el núcleo **sin Nuke**:

```
arw_probe DSC00001.ARW [--space 0-5] [--wb 0-3] [--hl 0-3] [--demosaic N]
                       [--ev F] [--half] [--nocrop] [--pfm salida.pfm]
```

Muestra los metadatos, el tiempo de revelado y, por canal, el mínimo, el máximo, la media y el % de píxeles por encima de 1.0. Opcionalmente escribe un PFM float, que Nuke puede abrir.

### 4.8 `docs/matrices.py` y `examples/arw_grade_example.nk`

El primero recalcula las matrices de color (por si quieres añadir otro espacio). El segundo es un script mínimo Read ARW → Grade → ColorCorrect → Write EXR 32 bits.

---

## 5. Compilación e instalación

### Requisitos

- **Nuke 14.0 o posterior** instalado (el NDK —cabeceras y `DDImage`— viene incluido con Nuke).
- **CMake ≥ 3.16** y **Git** (para descargar LibRaw).
- Compilador recomendado por Foundry para cada versión:

| Nuke | Windows | Linux |
|---|---|---|
| 14.x | Visual Studio 2019 (v142) | GCC 9.3, ABI antigua |
| 15.x | Visual Studio 2022 (v143) | GCC 11, ABI nueva |
| 16.x | Visual Studio 2022 (v143) | GCC 11, ABI nueva |

> Hay que compilar **un binario por versión mayor.menor** (14.0, 14.1, 15.0…). Es una limitación del NDK, no del plugin. El mismo código fuente vale para todas.

### Windows (Nuke 14)

```bat
build_windows.bat "C:\Program Files\Nuke14.0v5"
```

Para Nuke 15 o 16, añade el generador de VS2022:

```bat
build_windows.bat "C:\Program Files\Nuke15.1v3" "Visual Studio 17 2022"
```

El script compila e instala en `%USERPROFILE%\.nuke\arwReader\<versión>\`. Después, añade una vez esta línea a `%USERPROFILE%\.nuke\init.py`:

```python
nuke.pluginAddPath("arwReader")
```

### Linux / macOS

```bash
cmake -S . -B build-14.0 -DNUKE_ROOT=/usr/local/Nuke14.0v5
cmake --build build-14.0 -j
cmake --install build-14.0 --prefix ~/.nuke/arwReader
```

### Compilar solo la herramienta de prueba (sin Nuke)

```bash
cmake -S . -B build-probe -DARW_BUILD_PLUGIN=OFF -DARW_BUILD_PROBE=ON
cmake --build build-probe
./build-probe/arw_probe DSC00001.ARW
```

---

## 6. Uso en Nuke

1. **Leer**: crea un Read (o arrastra el `.ARW` al Node Graph). Aparecerá la sección *Sony RAW (LibRaw)* con los knobs de revelado.
2. **Espacio de color**:
   - Workflow por defecto de Nuke: `output space = linear Rec.709 (sRGB)` y `colorspace = linear`.
   - Workflow ACES: `output space = ACEScg` y `colorspace = ACES - ACEScg`.
   - Si `arw_colorspace.py` está activo, el *colorspace* del Read se ajusta solo.
3. **Retocar**: Grade, ColorCorrect, HueCorrect, etc. trabajan sobre el float completo. Por ejemplo, un Grade con `gain` 0.5 recupera las altas luces que en un JPEG estarían quemadas.
4. **Exportar** con el Write de siempre:
   - **EXR 16 bits half o 32 bits float** para conservar todo el rango (recomendado).
   - **DPX/TIFF 16 bits** o **JPEG/PNG 8 bits** para entregas finales. Elige en el Write el *colorspace* de salida (sRGB, Rec.709…), que es lo que aplica la curva y el recorte final.

**Consejos**

- `highlights = unclip` puede mostrar las zonas quemadas con un tinte magenta. Es la información real del sensor (uno de los canales saturó antes que los otros). Si prefieres blancos neutros, usa `clip` o `reconstruct`, o trátalo en Nuke.
- `half size` revela unas 3-4 veces más rápido. Úsalo para encuadrar y montar la cadena, y desactívalo para el render final.
- En una secuencia de ARW (time-lapse), el Read funciona como con cualquier secuencia (`DSC#####.ARW`).

---

## 7. Metadatos

Visibles con un nodo **ViewMetaData** y accesibles desde Python con `node.metadata()`:

| Clave | Contenido |
|---|---|
| `exif/0/Make`, `exif/0/Model` | Fabricante y modelo |
| `exif/2/ISOSpeedRatings`, `ExposureTime`, `FNumber`, `FocalLength` | Datos de disparo |
| `exif/2/LensModel`, `exif/2/DateTimeOriginal` | Objetivo y fecha |
| `input/bitsperchannel` | Profundidad real del sensor ("12-bit fixed", "14-bit fixed") |
| `arw/rawBitDepth`, `arw/blackLevel`, `arw/whiteLevel` | Niveles del sensor |
| `arw/cfaPattern`, `arw/sensorWidth`, `arw/sensorHeight`, `arw/orientation` | Geometría del sensor |
| `arw/asShotWB_R/G/B` | Multiplicadores de balance guardados por la cámara |
| `arw/hasColorMatrix` | 1 si LibRaw tiene matriz de color para la cámara |
| `arw/outputSpace` | Espacio de salida elegido |
| `arw/libraw` | Versión de LibRaw usada |

---

## 8. Verificación realizada

Durante el desarrollo, el núcleo se probó con `arw_probe` sobre un **ARW real de Sony A300 (12 bits)** y un NEF de Nikon D4 como segunda cámara:

- **Rango conservado**: en *unclip*, el rojo llega a 2.42 y el azul a 1.71. En *clip*, todo queda limitado a ~1.0, como debe ser.
- **Exactitud frente a referencia independiente** (rawpy/LibRaw desde Python):
  - modo cámara nativa: diferencia máxima 1·10⁻⁷ (redondeo float);
  - Rec.709 y ACEScg frente a matrices calculadas con numpy: diferencia máxima 1·10⁻⁵;
  - el blanco de cámara (1,1,1) se convierte exactamente en blanco (1,1,1).
- **Geometría**: el recorte de cámara (3872×2592) y el *half size* coinciden con el tamaño que anuncia la cabecera.
- **Profundidad**: se detectan 12 bits (blanco 4095).
- **Compilación**: CMake completo (descarga de LibRaw, núcleo, plugin y probe) en Linux.

**Lo que no se ha podido verificar** (requiere tu instalación de Nuke): la compilación contra el **NDK real** y la ejecución dentro de Nuke. El código del Reader solo se ha comprobado contra una imitación (mock) de la API DDImage. Tampoco se ha probado la rotación con un archivo vertical real ni un ARW de 14 bits real.

---

## 9. Limitaciones conocidas

- **Memoria**: una imagen revelada ocupa `ancho × alto × 12 bytes` (≈ 730 MB para 61 MP). El plugin libera la memoria de LibRaw en cuanto termina el revelado.
- **Primer acceso lento**: el revelado completo tarda entre ~1 y 4 s según la resolución y el algoritmo. Después, la caché de Nuke lo hace fluido.
- **Cámaras muy nuevas**: si LibRaw no reconoce un modelo reciente, sube `ARW_LIBRAW_TAG` a la última versión y recompila.
- Si al cambiar `half size` o `camera crop` el tamaño no se actualiza, pulsa **Reload** en el Read.
- La salida `CIE XYZ D65` no existe en la mayoría de configs OCIO. En ese caso, pon el *colorspace* del Read en `raw` a mano.

---

## 10. Licencias

LibRaw se distribuye con doble licencia, **LGPL 2.1 o CDDL 1.0**, a elección del usuario. Este plugin la enlaza estáticamente. Para uso interno no hay obligación práctica; si redistribuyes el binario, acogerse a la **CDDL** es lo más sencillo con enlace estático. Incluye su aviso de copyright (`COPYRIGHT` en el repositorio de LibRaw).
