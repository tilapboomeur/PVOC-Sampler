// Tests du moteur de l'Effet PVoc (sans JUCE).
#include "../Source/Effect/SpectralFx.h"

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <functional>
#include <random>
#include <vector>

using pvoc::SpectralFx;
static const double sr = 48000.0;
static const double PI = 3.14159265358979323846;
static int fails = 0;
static void check(bool ok, const char* what, double v) {
    std::printf("%s %s (%.4g)\n", ok ? "[OK]  " : "[ECHEC]", what, v);
    if (!ok) ++fails;
}

using Sig = std::function<float(long)>;
/** Fait passer n echantillons par blocs de tailles variables ; renvoie le canal gauche. */
static std::vector<float> run(SpectralFx& fx, long n, Sig in, Sig sc = nullptr, long t0 = 0) {
    std::vector<float> out;
    std::vector<float> a(700), b(700), c(700), l(700), r(700);
    std::mt19937 g(3);
    long p = 0;
    while (p < n) {
        const int k = (int) std::min<long>(n - p, 1 + (long) (g() % 700));
        for (int i = 0; i < k; ++i) { a[(size_t) i] = in(t0 + p + i); b[(size_t) i] = a[(size_t) i]; c[(size_t) i] = sc ? sc(t0 + p + i) : 0.0f; }
        fx.process(a.data(), b.data(), sc ? c.data() : nullptr, sc ? c.data() : nullptr, l.data(), r.data(), k);
        out.insert(out.end(), l.begin(), l.begin() + k);
        p += k;
    }
    return out;
}
static double band(const std::vector<float>& x, size_t a, size_t b, double f) {   // amplitude a f (Goertzel fenetre)
    double re = 0, im = 0;
    for (size_t i = a; i < b; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2 * PI * (double) (i - a) / (double) (b - a));
        re += x[i] * w * std::cos(2 * PI * f * (double) i / sr);
        im += x[i] * w * std::sin(2 * PI * f * (double) i / sr);
    }
    return 4.0 * std::sqrt(re * re + im * im) / (double) (b - a);
}
static double freqOf(const std::vector<float>& x, size_t a, size_t b) {
    double first = -1, last = -1;
    int cnt = 0;
    for (size_t i = a + 1; i < b; ++i)
        if (x[i - 1] < 0 && x[i] >= 0) {
            const double t = (double) i - 1 + x[i - 1] / (x[i - 1] - x[i]);
            if (first < 0) first = t;
            last = t;
            ++cnt;
        }
    return cnt > 1 ? (cnt - 1) * sr / (last - first) : 0.0;
}
static double rms(const std::vector<float>& x, size_t a, size_t b) {
    double s = 0;
    for (size_t i = a; i < b; ++i) s += x[i] * x[i];
    return std::sqrt(s / (double) (b - a));
}
static Sig sine(double f, double a = 0.4) { return [=](long t) { return (float) (a * std::sin(2 * PI * f * t / sr)); }; }
static Sig saw(double f, double a = 0.3) {
    return [=](long t) {   // dent de scie limitee en bande (20 harmoniques)
        double s = 0;
        for (int h = 1; h <= 20 && h * f < 20000; ++h) s += std::sin(2 * PI * h * f * t / sr) / h;
        return (float) (a * s * 0.6);
    };
}

int main() {
    // 1) transparence et latence
    {
        SpectralFx fx;
        fx.prepare(sr);
        std::mt19937 g(1);
        std::normal_distribution<float> d(0, 0.2f);
        std::vector<float> noise(200000);
        for (auto& v : noise) v = d(g);
        auto out = run(fx, (long) noise.size(), [&](long t) { return noise[(size_t) t]; });
        const int L = SpectralFx::LATENCY;
        double e = 0, s = 0;
        for (size_t i = 20000; i < noise.size(); ++i) { const double dd = out[i] - noise[i - (size_t) L]; e += dd * dd; s += noise[i - (size_t) L] * noise[i - (size_t) L]; }
        check(10 * std::log10(e / s) < -60, "neutre : reconstruction exacte (ecart en dB)", 10 * std::log10(e / s));
        int best = 0;
        double bc = -1;
        for (int lag = 0; lag < 4096; ++lag) {
            double c = 0;
            for (size_t i = 20000; i < 60000; ++i) c += out[i + (size_t) lag] * noise[i];
            if (c > bc) { bc = c; best = lag; }
        }
        check(best == L, "latence mesuree = annoncee (2047 echantillons)", best);
    }
    // 2) pitch-shift
    for (double semi : { 7.0, -12.0, 12.0, 24.0, -24.0 }) {
        SpectralFx fx;
        fx.prepare(sr);
        SpectralFx::Params p;
        p.pitchSemis = (float) semi;
        fx.setParams(p);
        auto out = run(fx, (long) sr * 2, sine(440));
        const double f = freqOf(out, 24000, out.size()), want = 440 * std::pow(2, semi / 12);
        char msg[128];
        std::snprintf(msg, sizeof msg, "pitch %+g : %.2f Hz (attendu %.2f), niveau", semi, f, want);
        check(std::abs(f - want) < 0.3 && std::abs(rms(out, 24000, out.size()) * std::sqrt(2) - 0.4) < 0.04, msg, rms(out, 24000, out.size()) * std::sqrt(2));
    }
    {   // son harmonique : toutes les harmoniques suivent (+12 -> 220 devient 440, 660 devient 1320)
        SpectralFx fx;
        fx.prepare(sr);
        SpectralFx::Params p;
        p.pitchSemis = 12.0f;
        fx.setParams(p);
        auto out = run(fx, (long) sr * 2, saw(220));
        const double h1 = band(out, 24000, out.size(), 440), h3 = band(out, 24000, out.size(), 1320),
                     old = band(out, 24000, out.size(), 220), mid = band(out, 24000, out.size(), 660);
        check(h1 > 0.1 && h3 > 0.02 && old < 0.01 && mid < 0.01, "pitch +12 sur dent de scie 220 Hz : harmoniques a 440/880/1320", h1);
    }
    // 3) decalage de frequence : +100 Hz sur 200 Hz harmonique -> 300, 500, 700 (inharmonique)
    {
        SpectralFx fx;
        fx.prepare(sr);
        SpectralFx::Params p;
        p.fshiftHz = 100.0f;
        fx.setParams(p);
        auto out = run(fx, (long) sr * 2, saw(200));
        const double a300 = band(out, 24000, out.size(), 300), a500 = band(out, 24000, out.size(), 500), a700 = band(out, 24000, out.size(), 700),
                     a200 = band(out, 24000, out.size(), 200), a400 = band(out, 24000, out.size(), 400);
        check(a300 > 0.1 && a500 > 0.03 && a700 > 0.02 && a200 < 0.01 && a400 < 0.01, "decalage +100 Hz : 200/400/600 -> 300/500/700", a300);
        SpectralFx fx2;
        fx2.prepare(sr);
        p.fshiftHz = -150.0f;
        fx2.setParams(p);
        out = run(fx2, (long) sr * 2, sine(440));
        check(std::abs(freqOf(out, 24000, out.size()) - 290) < 0.5, "decalage -150 Hz : 440 -> 290 Hz", freqOf(out, 24000, out.size()));
    }
    // 4) gel
    {
        SpectralFx fx;
        fx.prepare(sr);
        SpectralFx::Params p;
        fx.setParams(p);
        Sig in = [](long t) { return (float) (0.4 * std::sin(2 * PI * (t < 48000 ? 440 : 660) * t / sr)); };
        run(fx, 40000, in);
        p.freeze = true;
        fx.setParams(p);
        auto out = run(fx, (long) sr * 3, in, nullptr, 40000);
        const double f = freqOf(out, 24000, out.size());
        double lo = 1e9, hi = 0;
        for (size_t i = 24000; i + 4800 < out.size(); i += 4800) { const double r = rms(out, i, i + 4800); lo = std::min(lo, r); hi = std::max(hi, r); }
        check(std::abs(f - 440) < 0.3, "gel : 440 Hz tenu alors que l'entree passe a 660 Hz", f);
        check(hi / lo < 1.05 && std::abs(hi * std::sqrt(2) - 0.4) < 0.04, "gel : niveau stable sur 3 s (rapport max/min)", hi / lo);
        p.freeze = false;
        p.pitchSemis = 12.0f;
        fx.setParams(p);
        run(fx, 24000, in, nullptr, 176000);   // degel : l'entree est maintenant a 660 Hz
        p.freeze = true;
        fx.setParams(p);
        out = run(fx, (long) sr, in, nullptr, 200000);
        check(std::abs(freqOf(out, 12000, out.size()) - 1320) < 0.5, "gel + pitch +12 : la trame gelee se transpose (1320 Hz)", freqOf(out, 12000, out.size()));
    }
    // 5) harmoniseur : notes MIDI -> voix transposees
    {
        SpectralFx fx;
        fx.prepare(sr);
        SpectralFx::Params p;
        fx.setParams(p);
        SpectralFx::VoiceIn v[2];
        v[0].gate = true; v[0].semis = 4; v[0].level = 1;
        v[1].gate = true; v[1].semis = 7; v[1].level = 1;
        fx.setVoices(v, 2, true, false);
        auto out = run(fx, (long) sr * 2, sine(440));
        const double a0 = band(out, 24000, out.size(), 440), a4 = band(out, 24000, out.size(), 440 * std::pow(2, 4 / 12.0)),
                     a7 = band(out, 24000, out.size(), 440 * std::pow(2, 7 / 12.0));
        check(a0 > 0.35 && a4 > 0.35 && a7 > 0.35, "harmoniseur : 440 + tierce + quinte", std::min({ a0, a4, a7 }));
        p.keepMain = false;
        fx.setParams(p);
        out = run(fx, (long) sr * 2, sine(440));
        check(band(out, 24000, out.size(), 440) < 0.01 && band(out, 24000, out.size(), 554.37) > 0.35, "harmoniseur sans voix principale", band(out, 24000, out.size(), 440));
        v[0].gate = v[1].gate = false;
        fx.setVoices(v, 2, true, false);
        out = run(fx, (long) sr * 2, sine(440));
        check(band(out, 48000, out.size(), 554.37) < 0.005 && band(out, 48000, out.size(), 440) > 0.35, "notes relachees : retour au son seul", band(out, 48000, out.size(), 440));
        check(fx.activeVoices() == 0, "voix liberees", fx.activeVoices());
    }
    // 6) robot : bruit -> peigne harmonique a 110 Hz ; vocodeur au clavier (220 Hz)
    {
        std::mt19937 g(2);
        std::normal_distribution<float> d(0, 0.2f);
        std::vector<float> noise(200000);
        for (auto& x : noise) x = d(g);
        SpectralFx fx;
        fx.prepare(sr);
        SpectralFx::Params p;
        p.robot = 1.0f;
        p.robotHz = 110.0f;
        fx.setParams(p);
        auto out = run(fx, (long) noise.size(), [&](long t) { return noise[(size_t) t]; });
        double harm = 0, between = 0;
        for (int h = 2; h <= 12; ++h) { harm += band(out, 24000, 120000, 110.0 * h); between += band(out, 24000, 120000, 110.0 * h + 55.0); }
        check(harm > 8 * between, "robot 110 Hz : peigne harmonique (rapport harmoniques / entre-deux)", harm / std::max(1e-9, between));
        check(rms(out, 24000, 120000) > 0.05, "robot : niveau comparable a l'entree", rms(out, 24000, 120000));
        SpectralFx::VoiceIn v[1];
        v[0].gate = true; v[0].robotHz = 220.0f; v[0].level = 1;
        fx.setVoices(v, 1, false, true);
        out = run(fx, 96000, [&](long t) { return noise[(size_t) t]; });
        double h220 = 0, h110odd = 0;
        for (int h = 1; h <= 8; ++h) { h220 += band(out, 24000, 96000, 220.0 * h); h110odd += band(out, 24000, 96000, 220.0 * h - 110.0); }
        check(h220 > 6 * h110odd, "vocodeur au clavier : peigne a 220 Hz (note jouee)", h220 / std::max(1e-9, h110odd));
    }
    // 7) cross-synthese : porteuse riche (dent de scie 110 Hz), modulateur sinus 1500 Hz en sidechain
    {
        SpectralFx fx;
        fx.prepare(sr);
        SpectralFx::Params p;
        p.cross = 1.0f;
        p.crossSmooth = 0.25f;
        fx.setParams(p);
        auto out = run(fx, (long) sr * 2, saw(110), sine(1500, 0.5));
        const double near = band(out, 24000, out.size(), 1540), far = band(out, 24000, out.size(), 220);
        auto ref = run(*new SpectralFx(), (long) sr * 2, saw(110));
        const double rnear = band(ref, 24000, ref.size(), 1540), rfar = band(ref, 24000, ref.size(), 220);
        check((near / far) > 30 * (rnear / rfar), "cross-synthese : l'enveloppe du sidechain (1500 Hz) est imposee", (near / far) / (rnear / rfar));
        check(std::abs(freqOf(out, 24000, out.size()) - 1540) < 30 || near > 0.05, "cross-synthese : les harmoniques de la porteuse pres de 1500 Hz ressortent", near);
        // fichier en boucle comme modulateur
        auto pb = std::make_shared<pvoc::PlayerBuffer>();
        pb->length = 48000;
        pb->left.assign((size_t) (48000 + 2 * pvoc::Engine::PAD + 8), 0.0f);
        for (int i = 0; i < 48000; ++i) pb->left[(size_t) (pvoc::Engine::PAD + i)] = 0.5f * (float) std::sin(2 * PI * 1500 * i / sr);
        pb->right = pb->left;
        SpectralFx fx2;
        fx2.prepare(sr);
        p.crossSource = 1;
        fx2.setParams(p);
        fx2.setModulatorFile(pb);
        out = run(fx2, (long) sr * 2, saw(110));
        check(band(out, 24000, out.size(), 1540) / band(out, 24000, out.size(), 220) > 30 * (rnear / rfar), "cross-synthese avec un fichier en boucle", band(out, 24000, out.size(), 1540));
        check(fx2.modLevel.load() > 0.2f, "niveau du modulateur visible", fx2.modLevel.load());
    }
    // 8) porte spectrale : sinus + bruit faible
    {
        std::mt19937 g(4);
        std::normal_distribution<float> d(0, 0.01f);
        std::vector<float> nz(144000);
        for (auto& x : nz) x = d(g);
        Sig in = [&](long t) { return (float) (0.4 * std::sin(2 * PI * 440 * t / sr)) + nz[(size_t) t]; };
        auto noiseLevel = [&](const std::vector<float>& o) {
            double s = 0;
            for (double f : { 1234.0, 3333.0, 5555.0, 7777.0, 9999.0 }) s += band(o, 24000, o.size(), f);
            return s;
        };
        SpectralFx a, b;
        a.prepare(sr);
        b.prepare(sr);
        SpectralFx::Params p;
        p.gateDb = -30.0f;
        b.setParams(p);
        auto oa = run(a, 144000, in), ob = run(b, 144000, in);
        const double red = 20 * std::log10(noiseLevel(oa) / std::max(1e-12, noiseLevel(ob)));
        check(red > 20 && std::abs(band(ob, 24000, ob.size(), 440) - 0.4) < 0.03, "porte spectrale -30 dB : bruit reduit (dB), sinus intact", red);
    }
    // 9) flou spectral : clics toutes les 0,5 s -> l'energie s'etale entre les clics
    {
        Sig clicks = [](long t) { return (t % 24000) < 48 ? 0.8f : 0.0f; };
        SpectralFx a, b;
        a.prepare(sr);
        b.prepare(sr);
        SpectralFx::Params p;
        p.blur = 0.6f;   // constante de temps ~0,6 s
        b.setParams(p);
        auto oa = run(a, 144000, clicks), ob = run(b, 144000, clicks);
        // part de l'energie situee loin des clics (entre 190 et 420 ms apres chaque clic)
        auto tailShare = [](const std::vector<float>& o) {
            double tail = 0, all = 0;
            for (size_t i = 72000; i < 120000; ++i) {
                const double e = (double) o[i] * o[i];
                all += e;
                const long ph = (long) ((i - 2047) % 24000);
                if (ph >= 9000 && ph < 20000) tail += e;
            }
            return tail / (all + 1e-30);
        };
        check(tailShare(oa) < 0.01, "sans flou : l'energie reste sur les clics", tailShare(oa));
        check(tailShare(ob) > 0.05, "flou spectral : l'energie se prolonge entre les clics", tailShare(ob));
        const double early = rms(ob, 96000 + 2047 + 2400, 96000 + 2047 + 4800), late = rms(ob, 96000 + 2047 + 19200, 96000 + 2047 + 21600);
        check(late > 0.4 * early, "flou spectral : traine lente (niveau a 400 ms / a 50 ms)", late / early);
    }
    // 10) chuchotement : meme bande de frequences, son devenu bruite
    {
        SpectralFx a;
        a.prepare(sr);
        SpectralFx::Params p;
        p.whisper = 1.0f;
        a.setParams(p);
        auto o = run(a, 144000, saw(220));
        const double inBand = band(o, 24000, o.size(), 220) + band(o, 24000, o.size(), 440);
        check(rms(o, 24000, o.size()) > 0.05 && inBand < 0.12, "chuchotement : energie conservee, hauteur effacee", inBand);
    }
    // 11) melange sec / traite
    {
        SpectralFx a;
        a.prepare(sr);
        SpectralFx::Params p;
        p.mix = 0.0f;
        p.pitchSemis = 7.0f;
        a.setParams(p);
        auto o = run(a, 96000, sine(440));
        check(std::abs(freqOf(o, 24000, o.size()) - 440) < 0.3, "melange 0 % : son d'origine (retarde de la latence)", freqOf(o, 24000, o.size()));
    }
    // 12) charge processeur : 16 voix d'harmoniseur + robot sur un son riche
    {
        SpectralFx fx;
        fx.prepare(sr);
        SpectralFx::Params p;
        p.robot = 0.5f;
        p.fshiftHz = 30.0f;
        p.blur = 0.3f;
        fx.setParams(p);
        SpectralFx::VoiceIn v[16];
        for (int i = 0; i < 16; ++i) { v[i].gate = true; v[i].semis = (float) (i - 8); v[i].robotHz = 110.0f * (1 + i * 0.1f); }
        fx.setVoices(v, 16, true, true);
        std::vector<float> a(512), l(512), r(512);
        Sig s = saw(110);
        for (int i = 0; i < 512; ++i) a[(size_t) i] = s(i);
        const auto t0 = std::chrono::steady_clock::now();
        const int blocks = (int) (10 * sr / 512);
        for (int b = 0; b < blocks; ++b) fx.process(a.data(), a.data(), nullptr, nullptr, l.data(), r.data(), 512);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("[INFO]  16 voix + robot : %.0f ms de calcul pour 10 s de son (%.1f %% d'un coeur)\n", ms, ms / 100.0);
    }
    std::printf("\n%s\n", fails ? "DES TESTS ECHOUENT" : "TOUS LES TESTS DE L'EFFET PASSENT");
    return fails ? 1 : 0;
}
