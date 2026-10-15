// RawDecoder.h
// -----------------------------------------------------------------------------
// Núcleo de revelado RAW independiente de Nuke.
//
// Convierte un archivo RAW (pensado para Sony .ARW, pero vale para cualquier
// formato que soporte LibRaw) en una imagen RGB float32 lineal, SIN clamp:
//
//   1. LibRaw desempaqueta el mosaico Bayer (12/14 bits), resta el nivel de
//      negro y normaliza al nivel de blanco del sensor.
//   2. El balance de blancos se aplica normalizado por el multiplicador MAYOR
//      (modo "highlight unclip"), de forma que ningún canal pasa de 65535 en
//      la etapa entera de LibRaw -> no se pierde nada por encima del blanco.
//   3. LibRaw hace el demosaicing y entrega RGB de cámara (sin matriz).
//   4. Aquí, en float, se deshace la normalización del paso 2, se aplica la
//      exposición, la matriz cámara->Rec.709 y la conversión al espacio de
//      salida elegido. Nada se recorta: valores >1.0 y negativos se conservan.
//
// No depende de DDImage, así que se puede compilar y probar fuera de Nuke
// (ver tools/arw_probe.cpp).
// -----------------------------------------------------------------------------
#pragma once

#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace arw {

// Algoritmos de demosaicing de LibRaw (valor = user_qual de LibRaw).
enum class Demosaic : int {
    Linear = 0,
    VNG    = 1,
    PPG    = 2,
    AHD    = 3,   // por defecto: buena calidad / velocidad
    DCB    = 4,
    DHT    = 11,
    AAHD   = 12,
};

enum class WhiteBalance : int {
    AsShot   = 0, // el que guardó la cámara
    Auto     = 1, // promedio gris de LibRaw
    Daylight = 2, // multiplicadores "daylight" de la matriz de la cámara
    Custom   = 3, // multiplicadores R, G, B introducidos por el usuario
};

// Cómo tratar los píxeles donde algún canal del sensor saturó.
enum class Highlights : int {
    Clip        = 0, // recorta al canal que antes satura (blancos neutros, pierde rango)
    Unclip      = 1, // conserva todo el rango (por defecto)
    Blend       = 2, // mezcla de LibRaw
    Reconstruct = 3, // reconstrucción de LibRaw (nivel 3..9, se usa 5)
};

enum class OutputSpace : int {
    CameraNative = 0, // RGB de cámara con balance aplicado, sin matriz
    LinearRec709 = 1, // primarios Rec.709/sRGB, blanco D65
    ACEScg       = 2, // AP1, blanco ACES (~D60), Bradford desde D65
    ACES2065_1   = 3, // AP0, blanco ACES
    LinearRec2020= 4, // primarios Rec.2020, D65
    XYZ_D65      = 5, // CIE XYZ, blanco D65
};

struct DecodeOptions {
    Demosaic     demosaic      = Demosaic::AHD;
    WhiteBalance whiteBalance  = WhiteBalance::AsShot;
    float        customMul[3]  = {1.f, 1.f, 1.f}; // R, G, B (solo WB Custom)
    Highlights   highlights    = Highlights::Unclip;
    float        exposureStops = 0.f;   // multiplica por 2^stops
    OutputSpace  outputSpace   = OutputSpace::LinearRec709;
    bool         halfSize      = false; // proxy rápido: sin demosaic, mitad de resolución
    bool         applyCrop     = true;  // recorte "inset" que define la cámara
};

using MetaValue = std::variant<int, double, std::string>;
using MetaList  = std::vector<std::pair<std::string, MetaValue>>;

struct ImageInfo {
    int      width       = 0;   // tamaño FINAL (tras recorte, half-size y orientación)
    int      height      = 0;
    double   pixelAspect = 1.0;
    int      rawBits     = 0;   // profundidad real de los datos del sensor (12, 14...)
    MetaList metadata;          // claves estilo "exif/..." y "arw/..."
};

struct Image {
    int width  = 0;
    int height = 0;
    // RGB float32 entrelazado, fila 0 = ARRIBA de la imagen.
    std::vector<float> rgb;
};

class RawDecoder {
public:
    // Solo cabecera: rápido. Rellena tamaño final y metadatos.
    static bool readInfo(const std::string& utf8Path, const DecodeOptions& opt,
                         ImageInfo& info, std::string& error);

    // Revelado completo.
    static bool decode(const std::string& utf8Path, const DecodeOptions& opt,
                       Image& image, ImageInfo& info, std::string& error);

    // Versión de LibRaw enlazada (para metadatos / diagnóstico).
    static const char* librawVersion();
};

} // namespace arw
