#include "../Source/PvocEngine.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <random>
using pvoc::Engine;
static std::vector<float> run(Engine& e, const std::vector<float>& in, int n, std::vector<float>* outR = nullptr, int freezeAt = -1) {
    std::vector<float> out(n), r(n); int pos = 0; srand(7);
    while (pos < n) { int b = 1 + rand() % 700; if (b > n - pos) b = n - pos;
        if (freezeAt >= 0 && pos <= freezeAt && pos + b > freezeAt) e.setFreeze(true);
        e.process(in.data() + pos, in.data() + pos, out.data() + pos, r.data() + pos, b); pos += b; }
    if (outR) *outR = r; return out;
}
static double freqOf(const std::vector<float>& x, int a, int b, double sr) {
    // passages par zero interpoles
    double first = -1, last = -1; int cnt = 0;
    for (int i = a + 1; i < b; ++i) if (x[i-1] < 0 && x[i] >= 0) { double t = i - 1 + x[i-1] / (x[i-1] - x[i]); if (first < 0) first = t; last = t; ++cnt; }
    return (cnt - 1) * sr / (last - first);
}
static double rms(const std::vector<float>& x, int a, int b) { double s = 0; for (int i = a; i < b; ++i) s += x[i] * x[i]; return std::sqrt(s / (b - a)); }
int main() {
    const double sr = 48000; const int n = (int) (sr * 3);
    // 1) latence : bruit blanc, transposition nulle, 100 % traite
    { std::mt19937 g(1); std::normal_distribution<float> d(0, 0.2f); std::vector<float> in(n); for (auto& v : in) v = d(g);
      Engine e; e.setGain(1); e.setMix(1); e.setLock(false); e.prepare(sr); e.setMode(Engine::Mode::Effect);
      auto out = run(e, in, n); int best = 0; double bc = -1;
      for (int lag = 0; lag < 8000; ++lag) { double c = 0; for (int i = 20000; i < 60000; ++i) c += out[i + lag] * in[i]; if (c > bc) { bc = c; best = lag; } }
      std::printf("latence mesuree %d ech, annoncee %d ech (%.1f ms a 48 kHz)\n", best, e.latencySamples(), e.latencySamples() * 1000 / sr);
      double err = 0, ref = 0; for (int i = 20000; i < n - 8000; ++i) { double dd = out[i + best] - in[i]; err += dd * dd; ref += in[i] * in[i]; }
      std::printf("  reconstruction sans transposition : ecart %.1f dB\n", 10 * std::log10(err / ref)); }
    // 2) hauteur en direct
    for (double semi : {7.0, -12.0, 12.0, 24.0, -24.0}) {
      std::vector<float> in(n); for (int i = 0; i < n; ++i) in[i] = 0.4f * std::sin(2 * M_PI * 440 * i / sr);
      Engine e; e.setGain(1); e.setMix(1); e.prepare(sr); e.setMode(Engine::Mode::Effect); e.setTranspose(semi);
      auto out = run(e, in, n); double f = freqOf(out, n / 2, n - 4800, sr);
      std::printf("transpo %+5.1f : %.2f Hz (attendu %.2f), niveau %.3f\n", semi, f, 440 * std::pow(2, semi / 12), rms(out, n / 2, n - 4800) * std::sqrt(2)); }
    // 3) gel : sinus 330 Hz puis 550 Hz ; gel a 1 s -> la sortie doit rester a 330 Hz
    { std::vector<float> in(n); for (int i = 0; i < n; ++i) in[i] = 0.4f * std::sin(2 * M_PI * (i < sr * 1.3 ? 330 : 550) * i / sr);
      Engine e; e.setGain(1); e.setMix(1); e.prepare(sr); e.setMode(Engine::Mode::Effect);
      auto out = run(e, in, n, nullptr, (int) sr);
      std::printf("gel : apres 2 s %.2f Hz (attendu 330, l'entree est a 550), niveau %.3f\n", freqOf(out, (int) (sr * 2), n - 100, sr), rms(out, (int) (sr * 2), n) * std::sqrt(2)); }
    // 4) melange sec : mix 0 -> entree retardee exactement de la latence
    { std::vector<float> in(n); for (int i = 0; i < n; ++i) in[i] = std::sin(i * 0.013f) * 0.3f;
      Engine e; e.setGain(1); e.setMix(0); e.prepare(sr); e.setMode(Engine::Mode::Effect);
      auto out = run(e, in, n); int L = e.latencySamples(); double mx = 0;
      for (int i = L + 10; i < n; ++i) mx = std::fmax(mx, std::fabs(out[i] - in[i - L]));
      std::printf("mix 0 : ecart max avec l'entree retardee de %d ech = %.2e\n", L, mx); }
    // 5) lecteur : tailles de blocs aleatoires vs blocs fixes -> identique
    { std::vector<float> in(n); for (int i = 0; i < n; ++i) in[i] = std::sin(i * 0.05f) * 0.3f;
      auto pb = std::make_shared<pvoc::PlayerBuffer>(); pb->length = n; pb->left.assign(n + 2 * Engine::PAD + 8, 0); for (int i = 0; i < n; ++i) pb->left[Engine::PAD + i] = in[i]; pb->right = pb->left;
      Engine a, b; for (Engine* e : {&a, &b}) { e->setGain(1); e->prepare(sr); e->setTranspose(5); e->setSpeed(0.7); e->setPlayerBuffer(pb); e->play(); }
      std::vector<float> oa(n), ob(n), r(n), z(n, 0.f);
      for (int p = 0; p < n; p += 64) a.process(z.data(), z.data(), oa.data() + p, r.data() + p, 64);
      int p = 0; srand(3); while (p < n) { int k = 1 + rand() % 1000; if (k > n - p) k = n - p; b.process(z.data(), z.data(), ob.data() + p, r.data() + p, k); p += k; }
      double mx = 0; for (int i = 0; i < n; ++i) mx = std::fmax(mx, std::fabs(oa[i] - ob[i]));
      std::printf("lecteur : blocs de 64 vs blocs aleatoires, ecart max %.2e\n", mx); }
}
