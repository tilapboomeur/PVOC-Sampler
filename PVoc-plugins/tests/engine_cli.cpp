// Banc de test : lit du float32 stereo entrelace, joue avec les reglages donnes, ecrit la sortie.
#include "../Source/PvocEngine.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
int main(int argc, char** argv) {
    if (argc < 8) { std::fprintf(stderr, "usage: in.f32 out.f32 mode(0/1) speed transpo loop seconds [freezeAt] [lock]\n"); return 1; }
    FILE* f = std::fopen(argv[1], "rb"); std::vector<float> in;
    float buf[4096]; size_t r; while ((r = std::fread(buf, 4, 4096, f)) > 0) in.insert(in.end(), buf, buf + r); std::fclose(f);
    const int frames = (int) in.size() / 2;
    const int mode = std::atoi(argv[3]); const double speed = std::atof(argv[4]), transpo = std::atof(argv[5]);
    const bool loop = std::atoi(argv[6]); const double secs = std::atof(argv[7]);
    const double freezeAt = argc > 8 ? std::atof(argv[8]) : -1; const bool lock = argc > 9 ? std::atoi(argv[9]) : 1;
    pvoc::Engine e; e.prepare(44100);
    e.setSpeed(speed); e.setTranspose(transpo); e.setLoop(loop); e.setGain(1.0f); e.setLock(lock); e.prepare(44100);
    auto pb = std::make_shared<pvoc::PlayerBuffer>();
    pb->length = frames; pb->left.assign(frames + 2 * pvoc::Engine::PAD + 8, 0); pb->right = pb->left;
    for (int i = 0; i < frames; ++i) { pb->left[pvoc::Engine::PAD + i] = in[2 * i]; pb->right[pvoc::Engine::PAD + i] = in[2 * i + 1]; }
    std::vector<float> outBuf;
    if (mode == 0) { e.setPlayerBuffer(pb); e.play(); } else e.setMode(pvoc::Engine::Mode::Effect);
    const int total = (int) (secs * 44100); int pos = 0; int endAt = -1;
    std::vector<float> il(512), ir(512), ol(512), orr(512);
    srand(1);
    while (pos < total) {
        int n = 1 + rand() % 512; if (n > total - pos) n = total - pos;   // tailles de blocs aleatoires
        for (int i = 0; i < n; ++i) { int s = pos + i; il[i] = s < frames ? in[2*s] : 0; ir[i] = s < frames ? in[2*s+1] : 0; }
        if (freezeAt >= 0 && pos <= freezeAt * 44100 && pos + n > freezeAt * 44100) e.setFreeze(true);
        e.process(il.data(), ir.data(), ol.data(), orr.data(), n);
        for (int i = 0; i < n; ++i) { outBuf.push_back(ol[i]); outBuf.push_back(orr[i]); }
        if (e.takeEndFlag() && endAt < 0) endAt = pos + n;
        pos += n;
    }
    if (endAt >= 0) std::printf("END_AFTER_MS: %.1f\n", endAt * 1000.0 / 44100);
    FILE* o = std::fopen(argv[2], "wb"); std::fwrite(outBuf.data(), 4, outBuf.size(), o); std::fclose(o);
}
