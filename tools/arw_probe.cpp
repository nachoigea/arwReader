// arw_probe — prueba del núcleo de revelado SIN Nuke.
//
//   arw_probe archivo.ARW [--space N] [--wb N] [--hl N] [--demosaic N]
//                         [--ev F] [--half] [--nocrop] [--pfm salida.pfm]
//
// Imprime metadatos y estadísticas por canal (mínimo, máximo, % de píxeles
// por encima de 1.0) y opcionalmente escribe un PFM float para inspección.
#include "RawDecoder.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

using namespace arw;

static void printValue(const MetaValue& v) {
    if (auto i = std::get_if<int>(&v))         std::printf("%d", *i);
    else if (auto d = std::get_if<double>(&v)) std::printf("%g", *d);
    else std::printf("%s", std::get<std::string>(v).c_str());
}

static bool writePFM(const char* path, const Image& img) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fprintf(f, "PF\n%d %d\n-1.0\n", img.width, img.height);
    // PFM guarda las filas de abajo arriba (igual que Nuke).
    for (int y = img.height - 1; y >= 0; --y)
        std::fwrite(&img.rgb[size_t(y) * img.width * 3], sizeof(float), size_t(img.width) * 3, f);
    std::fclose(f);
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "uso: %s archivo.ARW [opciones]\n", argv[0]); return 2; }
    DecodeOptions o;
    const char* pfm = nullptr;
    for (int i = 2; i < argc; ++i) {
        auto next = [&]() { return i + 1 < argc ? argv[++i] : "0"; };
        if (!std::strcmp(argv[i], "--space"))         o.outputSpace  = OutputSpace(std::atoi(next()));
        else if (!std::strcmp(argv[i], "--wb"))       o.whiteBalance = WhiteBalance(std::atoi(next()));
        else if (!std::strcmp(argv[i], "--hl"))       o.highlights   = Highlights(std::atoi(next()));
        else if (!std::strcmp(argv[i], "--demosaic")) o.demosaic     = Demosaic(std::atoi(next()));
        else if (!std::strcmp(argv[i], "--ev"))       o.exposureStops = float(std::atof(next()));
        else if (!std::strcmp(argv[i], "--half"))     o.halfSize = true;
        else if (!std::strcmp(argv[i], "--nocrop"))   o.applyCrop = false;
        else if (!std::strcmp(argv[i], "--pfm"))      pfm = next();
    }

    std::printf("LibRaw %s\n", RawDecoder::librawVersion());
    ImageInfo hdr; std::string err;
    if (!RawDecoder::readInfo(argv[1], o, hdr, err)) { std::fprintf(stderr, "readInfo: %s\n", err.c_str()); return 1; }

    auto t0 = std::chrono::steady_clock::now();
    Image img; ImageInfo info;
    if (!RawDecoder::decode(argv[1], o, img, info, err)) { std::fprintf(stderr, "decode: %s\n", err.c_str()); return 1; }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::printf("cabecera: %dx%d  revelado: %dx%d  (%.2fs)\n", hdr.width, hdr.height, img.width, img.height, secs);
    for (auto& kv : info.metadata) { std::printf("  %-28s ", kv.first.c_str()); printValue(kv.second); std::printf("\n"); }

    const size_t n = size_t(img.width) * img.height;
    for (int c = 0; c < 3; ++c) {
        float mn = std::numeric_limits<float>::max(), mx = -mn; size_t over = 0; double sum = 0;
        for (size_t i = 0; i < n; ++i) {
            float v = img.rgb[i * 3 + c];
            mn = v < mn ? v : mn; mx = v > mx ? v : mx; over += v > 1.f; sum += v;
        }
        std::printf("canal %c: min %.5f  max %.5f  media %.5f  >1.0: %.3f%%\n",
                    "RGB"[c], mn, mx, sum / n, 100.0 * over / n);
    }
    if (hdr.width != img.width || hdr.height != img.height) { std::fprintf(stderr, "ERROR: tamaño de cabecera != revelado\n"); return 1; }
    if (pfm && !writePFM(pfm, img)) { std::fprintf(stderr, "no se pudo escribir %s\n", pfm); return 1; }
    return 0;
}
