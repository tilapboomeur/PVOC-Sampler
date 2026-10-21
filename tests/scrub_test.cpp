// Tests du scrub (moteur seul).
#include "../Source/PvocEngine.h"
#include <cmath>
#include <cstdio>
#include <vector>
using pvoc::Engine;
static int fails = 0;
static void check(bool ok, const char* what, double v) { std::printf("%s %s (%.3f)\n", ok ? "[OK]  " : "[ECHEC]", what, v); if (!ok) ++fails; }
static double freqOf(const std::vector<float>& x, size_t a, size_t b, double sr) {
    double first = -1, last = -1; int cnt = 0;
    for (size_t i = a + 1; i < b; ++i) if (x[i-1] < 0 && x[i] >= 0) { double t = i - 1 + x[i-1] / (x[i-1] - x[i]); if (first < 0) first = t; last = t; ++cnt; }
    return cnt > 1 ? (cnt - 1) * sr / (last - first) : 0;
}
static double rms(const std::vector<float>& x, size_t a, size_t b) { double s = 0; for (size_t i = a; i < b; ++i) s += x[i] * x[i]; return std::sqrt(2 * s / (b - a)); }
int main() {
    const double sr = 48000;
    // fichier : 1 s a 300 Hz puis 1 s a 900 Hz
    const int len = 96000;
    auto pb = std::make_shared<pvoc::PlayerBuffer>(); pb->length = len;
    pb->left.assign(len + 2 * Engine::PAD + 8, 0.f);
    for (int i = 0; i < len; ++i) pb->left[Engine::PAD + i] = 0.4f * (float) std::sin(2 * M_PI * (i < 48000 ? 300 : 900) * i / sr);
    pb->right = pb->left;
    std::vector<float> z(256, 0.f), l(256), r(256);
    auto run = [&](Engine& e, int n, std::vector<float>* out) {
        for (int p = 0; p < n; p += 256) { e.process(z.data(), z.data(), l.data(), r.data(), 256); if (out) out->insert(out->end(), l.begin(), l.end()); }
    };
    // 1) lecteur en pause : le scrub doit etre audible et suivre le curseur
    {
        Engine e; e.setGain(1); e.prepare(sr); e.setPlayerBuffer(pb);
        std::vector<float> o; run(e, 4800, &o);
        check(rms(o, 0, o.size()) < 1e-4, "pause sans scrub : silence", rms(o, 0, o.size()));
        o.clear(); e.setScrub(true, 0.25); run(e, 24000, &o);
        check(std::fabs(freqOf(o, 9600, o.size(), sr) - 300) < 1, "scrub a 25 % : 300 Hz entendu", freqOf(o, 9600, o.size(), sr));
        o.clear(); e.setScrub(true, 0.75); run(e, 24000, &o);
        check(std::fabs(freqOf(o, 12000, o.size(), sr) - 900) < 1, "scrub a 75 % : 900 Hz entendu", freqOf(o, 12000, o.size(), sr));
        check(std::fabs(e.position() - 0.75) < 0.01, "position suit le curseur", e.position());
        // transition entendue : pendant le glissement on passe par la zone de transition
        o.clear(); e.setScrub(true, 0.25); run(e, 2400, &o);
        check(rms(o, 0, o.size()) > 0.1, "glissement audible (pas de coupure)", rms(o, 0, o.size()));
        o.clear(); e.setScrub(false, 0.25); run(e, 24000, &o);
        check(rms(o, 9600, o.size()) < 1e-4, "relache en pause : retour au silence", rms(o, 9600, o.size()));
        check(!e.isPlaying(), "toujours en pause", e.isPlaying());
    }
    // 2) lecteur en lecture a vitesse 0 : scrub puis le son reste fige a la nouvelle position
    {
        Engine e; e.setGain(1); e.prepare(sr); e.setPlayerBuffer(pb); e.setSpeed(0); e.seek(0.25); e.play();
        std::vector<float> o; run(e, 9600, nullptr);
        e.setScrub(true, 0.75); run(e, 24000, nullptr); e.setScrub(false, 0.75); run(e, 24000, &o);
        check(std::fabs(freqOf(o, 9600, o.size(), sr) - 900) < 1, "vitesse 0 : son fige a la nouvelle position", freqOf(o, 9600, o.size(), sr));
        check(rms(o, 9600, o.size()) > 0.3, "vitesse 0 : toujours audible", rms(o, 9600, o.size()));
    }
    // 3) lecteur en lecture normale : apres le scrub, la lecture reprend depuis la nouvelle position
    {
        Engine e; e.setGain(1); e.prepare(sr); e.setPlayerBuffer(pb); e.play(); run(e, 4800, nullptr);
        e.setScrub(true, 0.6); run(e, 24000, nullptr);
        const double p0 = e.position(); e.setScrub(false, 0.6); run(e, 24000, nullptr);
        check(std::fabs(p0 - 0.6) < 0.01, "lecture : position tenue pendant le scrub", p0);
        check(std::fabs(e.position() - 0.85) < 0.02, "lecture : reprise depuis la position (0,6 + 0,25)", e.position());
    }
    // 4) effet gele : voyager dans l'historique
    {
        Engine e; e.setGain(1); e.setMix(1); e.prepare(sr); e.setMode(Engine::Mode::Effect);
        // entree : 2 s a 500 Hz puis 2 s a 700 Hz, gel apres 4 s
        std::vector<float> in(256), o;
        long t = 0;
        auto feed = [&](int n, bool keep) { for (int p = 0; p < n; p += 256) { for (int i = 0; i < 256; ++i, ++t) in[i] = 0.4f * (float) std::sin(2 * M_PI * (t < 96000 ? 500 : 700) * t / sr);
            e.process(in.data(), in.data(), l.data(), r.data(), 256); if (keep) o.insert(o.end(), l.begin(), l.end()); } };
        feed(192000, false);
        e.setFreeze(true); feed(9600, false);
        check(e.historySeconds() > 3.5, "historique disponible apres 4 s (s)", e.historySeconds());
        feed(24000, true);
        check(std::fabs(freqOf(o, 4800, o.size(), sr) - 700) < 1, "gel : son de l'instant du gel (700 Hz)", freqOf(o, 4800, o.size(), sr));
        // reculer de ~3 s : on doit retrouver le 500 Hz
        const double frac = 1.0 - 3.0 / e.historySeconds();
        o.clear(); e.setScrub(true, frac); feed(24000, true);
        check(std::fabs(freqOf(o, 12000, o.size(), sr) - 500) < 1, "scrub dans le gel, 3 s en arriere : 500 Hz", freqOf(o, 12000, o.size(), sr));
        o.clear(); e.setScrub(false, frac); feed(24000, true);
        check(std::fabs(freqOf(o, 4800, o.size(), sr) - 500) < 1 && rms(o, 4800, o.size()) > 0.3, "gel : reste a la position choisie", freqOf(o, 4800, o.size(), sr));
        check(std::fabs(e.position() - frac) < 0.01, "position dans le gel", e.position());
        e.setFreeze(false); o.clear(); feed(24000, true);
        check(std::fabs(freqOf(o, 12000, o.size(), sr) - 700) < 1, "degel : retour au direct", freqOf(o, 12000, o.size(), sr));
    }
    // 5) temps de lissage
    {
        Engine e; e.setGain(1); e.prepare(sr); e.setPlayerBuffer(pb); e.setSpeed(0); e.seek(0.1); e.play();
        e.setScrubTime(0.0); run(e, 2048, nullptr);
        e.setScrub(true, 0.9); run(e, 1024, nullptr);
        check(std::fabs(e.position() - 0.9) < 0.001, "lissage 0 ms : saut immediat", e.position());
        e.setScrub(false, 0.9); run(e, 2048, nullptr);
        e.setScrubTime(1000.0); e.seek(0.1); run(e, 2048, nullptr);
        e.setScrub(true, 0.9);
        const int n = 24064;                                   // 47 trames ~ 0,501 s
        run(e, n, nullptr);
        const double hops = std::ceil(n / 512.0);
        const double expect = 0.1 + 0.8 * (1.0 - std::exp(-hops * 512.0 / (1.0 * sr)));
        check(std::fabs(e.position() - expect) < 0.02, "lissage 1000 ms : ~39 % du chemin apres 0,5 s", e.position());
        e.setScrubTime(1000.0); run(e, (int) (sr * 6), nullptr);
        check(std::fabs(e.position() - 0.9) < 0.01, "lissage 1000 ms : cible atteinte apres 6 s", e.position());
    }
    std::printf("\n%s\n", fails ? "DES TESTS ECHOUENT" : "TOUS LES TESTS DE SCRUB PASSENT");
    return fails ? 1 : 0;
}
