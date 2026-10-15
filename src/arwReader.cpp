// arwReader.cpp
// -----------------------------------------------------------------------------
// Reader nativo de Nuke (NDK / DDImage) para archivos RAW de Sony (.arw, .sr2,
// .srf). Envuelve el núcleo arw::RawDecoder y se encarga solo de lo propio de
// Nuke:
//
//   * ArwReaderFormat -> knobs que aparecen en el nodo Read al abrir un .arw
//   * ArwReader       -> tamaño/formato, metadatos y entrega de filas (engine)
//
// Los píxeles llegan al árbol como float32 lineal sin recortar, de modo que
// Grade, ColorCorrect, etc. trabajan sobre todo el rango del sensor y el Write
// nativo puede exportar a EXR, DPX, TIFF... como con cualquier otra fuente.
//
// Compatible con el NDK de Nuke 14, 15 y 16 (la API Reader no ha cambiado);
// hay que compilar un binario por versión mayor.menor de Nuke (ver CMakeLists).
// -----------------------------------------------------------------------------
#include "RawDecoder.h"

#include "DDImage/Reader.h"
#include "DDImage/Read.h"
#include "DDImage/Row.h"
#include "DDImage/Knobs.h"
#include "DDImage/LUT.h"
#include "DDImage/MetaData.h"

#include <cstring>
#include <mutex>
#include <string>

#ifdef _WIN32
#  include <io.h>
#  define ARW_CLOSE _close
#else
#  include <unistd.h>
#  define ARW_CLOSE ::close
#endif

using namespace DD::Image;

namespace {

// Etiquetas de los menús. El ORDEN debe coincidir con las tablas de valores.
const char* const kDemosaicNames[] = {"AHD", "AAHD", "DHT", "DCB", "PPG", "VNG", "linear", nullptr};
const arw::Demosaic kDemosaicValues[] = {arw::Demosaic::AHD, arw::Demosaic::AAHD, arw::Demosaic::DHT,
                                         arw::Demosaic::DCB, arw::Demosaic::PPG, arw::Demosaic::VNG,
                                         arw::Demosaic::Linear};

const char* const kWbNames[] = {"as shot", "auto", "daylight", "custom", nullptr};
const char* const kHlNames[] = {"unclip (conservar todo)", "clip", "blend", "reconstruct", nullptr};
const arw::Highlights kHlValues[] = {arw::Highlights::Unclip, arw::Highlights::Clip,
                                     arw::Highlights::Blend, arw::Highlights::Reconstruct};

const char* const kSpaceNames[] = {"linear Rec.709 (sRGB)", "ACEScg", "ACES2065-1",
                                   "linear Rec.2020", "CIE XYZ D65", "camera native", nullptr};
const arw::OutputSpace kSpaceValues[] = {arw::OutputSpace::LinearRec709, arw::OutputSpace::ACEScg,
                                         arw::OutputSpace::ACES2065_1, arw::OutputSpace::LinearRec2020,
                                         arw::OutputSpace::XYZ_D65, arw::OutputSpace::CameraNative};

} // namespace

// =============================================================================
//  ArwReaderFormat: parámetros de revelado expuestos en el nodo Read.
//  Los nombres llevan prefijo "arw_" para no chocar con knobs de Read ni de
//  otros readers (y para poder usarlos desde Python: read['arw_exposure']).
// =============================================================================
class ArwReaderFormat : public ReaderFormat {
public:
    int   space      = 0;          // índice en kSpaceNames
    int   wb         = 0;          // índice en kWbNames
    float customMul[3] = {2.0f, 1.0f, 1.5f};
    int   highlights = 0;
    int   demosaic   = 0;
    float exposure   = 0.f;
    bool  halfSize   = false;
    bool  applyCrop  = true;

    void knobs(Knob_Callback f) override {
        Divider(f, "Sony RAW (LibRaw)");

        Enumeration_knob(f, &space, kSpaceNames, "arw_space", "output space");
        Tooltip(f, "Espacio de color lineal en el que llegan los píxeles al árbol. "
                   "Pon el 'colorspace' del Read en el equivalente lineal "
                   "(linear / ACEScg / ACES2065-1...) o en 'raw' para que Nuke no lo transforme.");

        Float_knob(f, &exposure, IRange(-4, 4), "arw_exposure", "exposure (stops)");
        Tooltip(f, "Ajuste de exposición previo, en pasos (x2^stops). Es lineal y no recorta.");

        Enumeration_knob(f, &wb, kWbNames, "arw_wb", "white balance");
        Tooltip(f, "as shot: el de la cámara · auto: gris medio de LibRaw · "
                   "daylight: balance de luz de día de la cámara · custom: multiplicadores manuales.");
        Color_knob(f, customMul, IRange(0, 4), "arw_wb_mul", "custom multipliers");
        Tooltip(f, "Multiplicadores R, G, B para white balance = custom (se normalizan internamente).");

        Enumeration_knob(f, &highlights, kHlNames, "arw_highlights", "highlights");
        Tooltip(f, "unclip conserva TODO el rango (valores > 1.0 donde algún canal satura). "
                   "clip recorta al primer canal saturado (blancos neutros, menos rango).");

        Enumeration_knob(f, &demosaic, kDemosaicNames, "arw_demosaic", "demosaic");
        Tooltip(f, "Algoritmo de interpolación del mosaico Bayer. AHD es el equilibrio habitual.");

        Bool_knob(f, &halfSize, "arw_half_size", "half size (proxy)");
        Tooltip(f, "Revelado rápido a media resolución sin demosaicing. Útil para previsualizar.");
        SetFlags(f, Knob::STARTLINE);
        Bool_knob(f, &applyCrop, "arw_crop", "camera crop");
        Tooltip(f, "Aplica el recorte útil que define la cámara (quita los bordes del sensor).");
    }

    void append(Hash& hash) override {
        hash.append(space);
        hash.append(wb);
        hash.append(customMul[0]); hash.append(customMul[1]); hash.append(customMul[2]);
        hash.append(highlights);
        hash.append(demosaic);
        hash.append(exposure);
        hash.append(halfSize);
        hash.append(applyCrop);
    }

    arw::DecodeOptions options() const {
        arw::DecodeOptions o;
        o.outputSpace   = kSpaceValues[clampIndex(space, 6)];
        o.whiteBalance  = arw::WhiteBalance(clampIndex(wb, 4));
        o.customMul[0]  = customMul[0];
        o.customMul[1]  = customMul[1];
        o.customMul[2]  = customMul[2];
        o.highlights    = kHlValues[clampIndex(highlights, 4)];
        o.demosaic      = kDemosaicValues[clampIndex(demosaic, 7)];
        o.exposureStops = exposure;
        o.halfSize      = halfSize;
        o.applyCrop     = applyCrop;
        return o;
    }

private:
    static int clampIndex(int v, int n) { return v < 0 ? 0 : (v >= n ? n - 1 : v); }
};

// =============================================================================
//  ArwReader
// =============================================================================
class ArwReader : public Reader {
public:
    ArwReader(Read* r, int fd) : Reader(r) {
        // Nuke nos pasa el archivo abierto; LibRaw lo abre por ruta, así que
        // cerramos el descriptor para no dejarlo colgando.
        if (fd >= 0) ARW_CLOSE(fd);

        path_ = r->filename() ? r->filename() : "";
        if (auto* fmt = dynamic_cast<ArwReaderFormat*>(r->handler()))
            options_ = fmt->options();

        // Solo cabecera: Nuke necesita el tamaño antes de pedir píxeles.
        arw::ImageInfo info;
        std::string err;
        if (!arw::RawDecoder::readInfo(path_, options_, info, err)) {
            iop->error("arwReader: no se puede abrir '%s': %s", path_.c_str(), err.c_str());
            return;
        }
        headerOk_ = true;
        set_info(info.width, info.height, 3, info.pixelAspect);
        info_.ydirection(-1);   // leemos de arriba abajo de forma natural

        // Datos float lineales: el LUT por defecto del Read es "linear".
        lut_ = LUT::GetLut(LUT::FLOAT, this);

        fillMetaData(info);
    }

    void engine(int y, int x, int r, ChannelMask channels, Row& row) override {
        if (!ensureDecoded()) {
            row.erase(channels);
            return;
        }
        // Nuke numera las filas de abajo arriba; la imagen decodificada, de arriba abajo.
        const int srcY = image_.height - 1 - y;
        if (srcY < 0 || srcY >= image_.height) { row.erase(channels); return; }
        const float* src = &image_.rgb[(size_t(srcY) * image_.width) * 3];

        foreach (z, channels) {
            float* out = row.writable(z);
            const int c = (z == Chan_Red) ? 0 : (z == Chan_Green) ? 1 : (z == Chan_Blue) ? 2 : -1;
            if (c < 0) {
                for (int i = x; i < r; ++i) out[i] = 0.f;
                continue;
            }
            // from_float aplica el "colorspace" elegido en el Read (stride 3 = RGB entrelazado).
            from_float(z, out + x, src + size_t(x) * 3 + c, nullptr, r - x, 3);
        }
    }

    const MetaData::Bundle& fetchMetaData(const char* /*key*/) override { return meta_; }

    static const Description d;

private:
    // Revela el archivo una sola vez por Reader; engine() se llama desde
    // varios hilos a la vez, por eso el mutex.
    bool ensureDecoded() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (decoded_) return decodeOk_;
        decoded_ = true;
        if (!headerOk_) return decodeOk_ = false;

        arw::ImageInfo info;
        std::string err;
        decodeOk_ = arw::RawDecoder::decode(path_, options_, image_, info, err);
        if (!decodeOk_) {
            iop->error("arwReader: error revelando '%s': %s", path_.c_str(), err.c_str());
        } else if (image_.width != width() || image_.height != height()) {
            iop->error("arwReader: el tamaño revelado (%dx%d) no coincide con la cabecera (%dx%d)",
                       image_.width, image_.height, width(), height());
            decodeOk_ = false;
        }
        return decodeOk_;
    }

    void fillMetaData(const arw::ImageInfo& info) {
        for (const auto& kv : info.metadata) {
            const std::string& key = kv.first;
            if (auto i = std::get_if<int>(&kv.second))         meta_.setData(key, *i);
            else if (auto v = std::get_if<double>(&kv.second)) meta_.setData(key, *v);
            else meta_.setData(key, std::get<std::string>(kv.second));
        }
        // Profundidad REAL del sensor (12/14 bits); lo que llega al árbol es float 32.
        meta_.setData("input/bitsperchannel", std::to_string(info.rawBits) + "-bit fixed");
        meta_.setData("arw/outputSpace", std::string(kSpaceNames[clampSpace()]));
    }

    int clampSpace() const {
        for (int i = 0; i < 6; ++i) if (kSpaceValues[i] == options_.outputSpace) return i;
        return 0;
    }

    std::string        path_;
    arw::DecodeOptions options_;
    arw::Image         image_;
    MetaData::Bundle   meta_;
    std::mutex         mutex_;
    bool headerOk_ = false;
    bool decoded_  = false;
    bool decodeOk_ = false;
};

// ---------------------------------------------------------------------------
//  Registro en Nuke
// ---------------------------------------------------------------------------
static Reader* buildReader(Read* iop, int fd, const unsigned char*, int) {
    return new ArwReader(iop, fd);
}

static ReaderFormat* buildFormat(Read*) { return new ArwReaderFormat(); }

// Los ARW son contenedores TIFF: comprobamos la firma "II*\0" o "MM\0*".
static bool testFile(int, const unsigned char* block, int length) {
    if (length < 4) return false;
    return (block[0] == 'I' && block[1] == 'I' && block[2] == 0x2A && block[3] == 0x00) ||
           (block[0] == 'M' && block[1] == 'M' && block[2] == 0x00 && block[3] == 0x2A);
}

// Nuke busca el plugin "<extensión>Reader". Este binario se llama arwReader;
// para .sr2 / .srf se incluyen sr2Reader.tcl y srfReader.tcl que lo cargan.
const Reader::Description ArwReader::d("arw\0sr2\0srf\0", buildReader, testFile, buildFormat);
