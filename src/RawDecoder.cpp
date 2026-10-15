// RawDecoder.cpp — ver RawDecoder.h para la descripción del flujo.
#include "RawDecoder.h"

#include <libraw/libraw.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <memory>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace arw {
namespace {

// ---------------------------------------------------------------------------
// Matrices lineales Rec.709 (D65) -> espacio de salida.
// Calculadas con docs/matrices.py a partir de los primarios oficiales;
// los espacios ACES usan adaptación cromática Bradford D65 -> blanco ACES.
// ---------------------------------------------------------------------------
using M3 = float[3][3];

const M3 kIdentity = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
const M3 kRec709ToACEScg = {
    {0.6130974024f, 0.3395231462f, 0.0473794514f},
    {0.0701937225f, 0.9163538791f, 0.0134523985f},
    {0.0206155929f, 0.1095697729f, 0.8698146342f}};
const M3 kRec709ToAP0 = {
    {0.4396329819f, 0.3829886982f, 0.1773783199f},
    {0.0897764430f, 0.8134394287f, 0.0967841283f},
    {0.0175411704f, 0.1115465533f, 0.8709122763f}};
const M3 kRec709ToRec2020 = {
    {0.6274038959f, 0.3292830384f, 0.0433130657f},
    {0.0690972894f, 0.9195403951f, 0.0113623156f},
    {0.0163914389f, 0.0880133079f, 0.8955952532f}};
const M3 kRec709ToXYZ = {
    {0.4123907993f, 0.3575843394f, 0.1804807884f},
    {0.2126390059f, 0.7151686788f, 0.0721923154f},
    {0.0193308187f, 0.1191947798f, 0.9505321522f}};

const M3& outputMatrix(OutputSpace s) {
    switch (s) {
        case OutputSpace::ACEScg:        return kRec709ToACEScg;
        case OutputSpace::ACES2065_1:    return kRec709ToAP0;
        case OutputSpace::LinearRec2020: return kRec709ToRec2020;
        case OutputSpace::XYZ_D65:       return kRec709ToXYZ;
        default:                         return kIdentity;
    }
}

// Abre el archivo respetando rutas Unicode en Windows (LibRaw usa wchar_t ahí).
int openRaw(LibRaw& lr, const std::string& utf8Path) {
#if defined(_WIN32) && defined(LIBRAW_WIN32_UNICODEPATHS)
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(), -1, &w[0], n);
    return lr.open_file(w.c_str());
#else
    return lr.open_file(utf8Path.c_str());
#endif
}

// Configura LibRaw para entregar datos lineales de 16 bits SIN recorte ni
// conversión de color: todo lo "creativo" se hace después en float.
void configure(LibRaw& lr, const DecodeOptions& o) {
    libraw_output_params_t& p = lr.imgdata.params;
    p.output_bps      = 16;
    p.gamm[0]         = 1.0;   // curva lineal
    p.gamm[1]         = 1.0;
    p.no_auto_bright  = 1;     // sin auto-exposición
    p.output_color    = 0;     // RGB de cámara: la matriz la aplicamos en float
    p.user_qual       = static_cast<int>(o.demosaic);
    p.half_size       = o.halfSize ? 1 : 0;
    p.four_color_rgb  = 0;
    p.user_flip       = 0;     // la orientación la aplicamos nosotros
    p.use_camera_wb   = 0;
    p.use_auto_wb     = 0;
    for (float& m : p.user_mul) m = 0.f;

    switch (o.whiteBalance) {
        case WhiteBalance::AsShot:   p.use_camera_wb = 1; break;
        case WhiteBalance::Auto:     p.use_auto_wb = 1;   break;
        case WhiteBalance::Daylight: break; // pre_mul por defecto de LibRaw
        case WhiteBalance::Custom:
            p.user_mul[0] = std::max(o.customMul[0], 1e-4f);
            p.user_mul[1] = std::max(o.customMul[1], 1e-4f);
            p.user_mul[2] = std::max(o.customMul[2], 1e-4f);
            p.user_mul[3] = p.user_mul[1];
            break;
    }

    // highlight >= 1 hace que LibRaw divida los multiplicadores de WB por el
    // MAYOR, con lo que ningún canal desborda los 16 bits. Ese factor se
    // deshace luego en float (ver decode()). highlight == 0 recorta.
    switch (o.highlights) {
        case Highlights::Clip:        p.highlight = 0; break;
        case Highlights::Unclip:      p.highlight = 1; break;
        case Highlights::Blend:       p.highlight = 2; break;
        case Highlights::Reconstruct: p.highlight = 5; break;
    }
}

struct Geometry {
    int srcW = 0, srcH = 0;       // tamaño de imgdata.image (iwidth/iheight)
    int cropX = 0, cropY = 0;     // recorte en coordenadas de imgdata.image
    int cropW = 0, cropH = 0;
    int flip = 0;                 // 0, 3 (180º), 5 (90º antihorario), 6 (90º horario)
    int outW = 0, outH = 0;
};

Geometry computeGeometry(const LibRaw& lr, const DecodeOptions& o) {
    const libraw_image_sizes_t& S = lr.imgdata.sizes;
    Geometry g;
    const int shrink = (o.halfSize && lr.imgdata.idata.filters) ? 1 : 0;
    g.srcW = (S.width  + shrink) >> shrink;
    g.srcH = (S.height + shrink) >> shrink;
    g.cropX = 0; g.cropY = 0; g.cropW = g.srcW; g.cropH = g.srcH;

    if (o.applyCrop) {
        const libraw_raw_inset_crop_t& c = S.raw_inset_crops[0];
        // Las coordenadas del recorte van referidas al raw completo (con márgenes).
        // Muchas Sony indican el TAMAÑO del recorte pero no su posición
        // (0xFFFF = desconocida); en ese caso se centra en el área visible.
        int cw = c.cwidth, ch = c.cheight;
        int cx = (c.cleft == 0xffff) ? (S.width  - cw) / 2 : int(c.cleft) - int(S.left_margin);
        int cy = (c.ctop  == 0xffff) ? (S.height - ch) / 2 : int(c.ctop)  - int(S.top_margin);
        bool valid = cw > 0 && ch > 0 && cx >= 0 && cy >= 0 &&
                     cx + cw <= S.width && cy + ch <= S.height;
        if (valid) {
            g.cropX = cx >> shrink; g.cropY = cy >> shrink;
            g.cropW = std::min(cw >> shrink, g.srcW - g.cropX);
            g.cropH = std::min(ch >> shrink, g.srcH - g.cropY);
        }
    }

    g.flip = S.flip;
    const bool swap = (g.flip == 5 || g.flip == 6);
    g.outW = swap ? g.cropH : g.cropW;
    g.outH = swap ? g.cropW : g.cropH;
    return g;
}

int bitsFromWhite(unsigned white) {
    int b = 0;
    while (b < 32 && (1u << b) <= white) ++b;
    return b;
}

void fillMetadata(const LibRaw& lr, const Geometry& g, ImageInfo& info) {
    const auto& I = lr.imgdata.idata;
    const auto& O = lr.imgdata.other;
    const auto& C = lr.imgdata.color;
    const auto& L = lr.imgdata.lens;
    MetaList& m = info.metadata;
    m.clear();

    info.width       = g.outW;
    info.height      = g.outH;
    info.pixelAspect = lr.imgdata.sizes.pixel_aspect > 0 ? lr.imgdata.sizes.pixel_aspect : 1.0;
    info.rawBits     = bitsFromWhite(C.maximum);

    m.emplace_back("exif/0/Make",  std::string(I.make));
    m.emplace_back("exif/0/Model", std::string(I.model));
    if (O.iso_speed > 0) m.emplace_back("exif/2/ISOSpeedRatings", double(O.iso_speed));
    if (O.shutter   > 0) m.emplace_back("exif/2/ExposureTime",    double(O.shutter));
    if (O.aperture  > 0) m.emplace_back("exif/2/FNumber",         double(O.aperture));
    if (O.focal_len > 0) m.emplace_back("exif/2/FocalLength",     double(O.focal_len));
    if (L.Lens[0])                 m.emplace_back("exif/2/LensModel", std::string(L.Lens));
    else if (L.makernotes.Lens[0]) m.emplace_back("exif/2/LensModel", std::string(L.makernotes.Lens));
    if (O.timestamp > 0) {
        char buf[32];
        std::time_t t = O.timestamp;
        std::tm tmv{};
#ifdef _WIN32
        gmtime_s(&tmv, &t);
#else
        gmtime_r(&t, &tmv);
#endif
        std::strftime(buf, sizeof buf, "%Y:%m:%d %H:%M:%S", &tmv);
        m.emplace_back("exif/2/DateTimeOriginal", std::string(buf));
    }

    m.emplace_back("arw/rawBitDepth",   info.rawBits);
    m.emplace_back("arw/blackLevel",    int(C.black));
    m.emplace_back("arw/whiteLevel",    int(C.maximum));
    m.emplace_back("arw/cfaPattern",    std::string(I.cdesc));
    m.emplace_back("arw/sensorWidth",   int(lr.imgdata.sizes.raw_width));
    m.emplace_back("arw/sensorHeight",  int(lr.imgdata.sizes.raw_height));
    m.emplace_back("arw/orientation",   g.flip);
    m.emplace_back("arw/asShotWB_R",    double(C.cam_mul[0]));
    m.emplace_back("arw/asShotWB_G",    double(C.cam_mul[1]));
    m.emplace_back("arw/asShotWB_B",    double(C.cam_mul[2]));
    m.emplace_back("arw/hasColorMatrix", (C.cam_xyz[0][0] != 0.f) ? 1 : 0);
    m.emplace_back("arw/libraw",        std::string(LibRaw::version()));
}

} // namespace

const char* RawDecoder::librawVersion() { return LibRaw::version(); }

bool RawDecoder::readInfo(const std::string& path, const DecodeOptions& opt,
                          ImageInfo& info, std::string& error) {
    auto lr = std::make_unique<LibRaw>();   // LibRaw es grande: siempre en heap
    configure(*lr, opt);
    int rc = openRaw(*lr, path);
    if (rc != LIBRAW_SUCCESS) { error = libraw_strerror(rc); return false; }
    Geometry g = computeGeometry(*lr, opt);
    fillMetadata(*lr, g, info);
    return true;
}

bool RawDecoder::decode(const std::string& path, const DecodeOptions& opt,
                        Image& image, ImageInfo& info, std::string& error) {
    auto lr = std::make_unique<LibRaw>();
    configure(*lr, opt);

    int rc = openRaw(*lr, path);
    if (rc == LIBRAW_SUCCESS) rc = lr->unpack();
    if (rc != LIBRAW_SUCCESS) { error = libraw_strerror(rc); return false; }

    Geometry g = computeGeometry(*lr, opt);
    fillMetadata(*lr, g, info);

    rc = lr->dcraw_process();
    if (rc != LIBRAW_SUCCESS) { error = libraw_strerror(rc); return false; }

    const libraw_image_sizes_t& S = lr->imgdata.sizes;
    if (S.iwidth != g.srcW || S.iheight != g.srcH) {
        error = "tamaño inesperado tras el revelado";
        return false;
    }

    const libraw_colordata_t& C = lr->imgdata.color;

    // --- 1. Deshacer la normalización del WB --------------------------------
    // LibRaw dejó pre_mul[c] = mul_c / max(mul). Multiplicando por
    // 1/min(pre_mul) el canal con el multiplicador más bajo (normalmente G)
    // vuelve a tener su blanco de sensor en 1.0 y los demás pueden pasar de
    // 1.0: es justo la información que una conversión a 8 bits tiraría.
    float minMul = 1.f;
    for (int c = 0; c < 3; ++c)
        if (C.pre_mul[c] > 0.f) minMul = std::min(minMul, C.pre_mul[c]);
    // (En modo Clip LibRaw normaliza por el mínimo, así que minMul ya vale 1.)
    const float scale = std::pow(2.f, opt.exposureStops) / (65535.f * minMul);

    // --- 2. Matriz final = salida <- Rec.709 <- cámara -----------------------
    // rgb_cam es la matriz de LibRaw para RGB de cámara (balanceado) -> sRGB
    // lineal; cada fila suma 1, así que el blanco neutro se mantiene.
    float M[3][3];
    if (opt.outputSpace == OutputSpace::CameraNative) {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) M[i][j] = (i == j) ? scale : 0.f;
    } else {
        const M3& T = outputMatrix(opt.outputSpace);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                double v = 0.0;
                for (int k = 0; k < 3; ++k) v += double(T[i][k]) * C.rgb_cam[k][j];
                M[i][j] = float(v) * scale;
            }
    }

    // --- 3. Recorte + orientación + conversión a float ----------------------
    image.width  = g.outW;
    image.height = g.outH;
    image.rgb.assign(size_t(g.outW) * g.outH * 3, 0.f);

    const ushort (*src)[4] = lr->imgdata.image;
    for (int oy = 0; oy < g.outH; ++oy) {
        float* dst = &image.rgb[size_t(oy) * g.outW * 3];
        for (int ox = 0; ox < g.outW; ++ox, dst += 3) {
            // Coordenadas dentro del recorte sin rotar.
            int cx, cy;
            switch (g.flip) {
                case 3:  cx = g.cropW - 1 - ox; cy = g.cropH - 1 - oy; break; // 180º
                case 5:  cx = g.cropW - 1 - oy; cy = ox;               break; // 90º antihorario
                case 6:  cx = oy;               cy = g.cropH - 1 - ox; break; // 90º horario
                default: cx = ox;               cy = oy;               break;
            }
            const ushort* p = src[size_t(cy + g.cropY) * g.srcW + (cx + g.cropX)];
            const float r = p[0], gr = p[1], b = p[2];
            dst[0] = M[0][0] * r + M[0][1] * gr + M[0][2] * b;
            dst[1] = M[1][0] * r + M[1][1] * gr + M[1][2] * b;
            dst[2] = M[2][0] * r + M[2][1] * gr + M[2][2] * b;
        }
    }
    return true;
}

} // namespace arw
