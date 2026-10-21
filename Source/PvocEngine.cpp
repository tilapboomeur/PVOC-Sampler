#include "PvocEngine.h"

#include <algorithm>
#include <cmath>

namespace pvoc {

namespace {
constexpr double TWO_PI = 6.283185307179586476925286766559;

struct Twiddles {
    std::vector<float> cosT, sinT;
    std::vector<int> bitrev;
    Twiddles() {
        const int n = Engine::N;
        cosT.resize(n / 2);
        sinT.resize(n / 2);
        for (int k = 0; k < n / 2; ++k) {
            cosT[k] = (float) std::cos(TWO_PI * k / n);
            sinT[k] = (float) std::sin(TWO_PI * k / n);
        }
        bitrev.resize(n);
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b) r |= ((i >> b) & 1) << (bits - 1 - b);
            bitrev[i] = r;
        }
    }
};
const Twiddles& twiddles() {
    static const Twiddles t;
    return t;
}
} // namespace

Engine::Engine(bool withEffectBuffers) : hasEffectBuffers(withEffectBuffers) {
    hann.resize(N);
    for (int i = 0; i < N; ++i)   // comme le patch : 0.5 - 0.5 cos(2 pi n / N)
        hann[i] = (float) (0.5 - 0.5 * std::cos(6.283185307 * i / N));
    for (auto* v : {&wFL, &wFR, &wB, &reL, &imL, &reR, &imR, &reB, &imB})
        v->assign(N, 0.0f);
    for (auto* v : {&prevRe, &prevIm, &uRe, &uIm, &qRe, &qIm})
        v->assign(N / 2 + 1, 0.0f);
    olaL.assign(N, 0.0f);
    olaR.assign(N, 0.0f);
    if (hasEffectBuffers) {
        ringL.assign(2 * RING + 16, 0.0f);
        ringR.assign(2 * RING + 16, 0.0f);
        dryL.assign(EFFECT_LATENCY, 0.0f);
        dryR.assign(EFFECT_LATENCY, 0.0f);
    }
    twiddles();
    prepare(48000.0);
}

void Engine::prepare(double sampleRate) {
    sr = sampleRate > 0 ? sampleRate : 48000.0;
    rampCoef = (float) (1.0 - std::exp(-1.0 / (0.010 * sr)));   // lissage ~10 ms
    setScrubTime(scrubMs);                                          // glissement du scrub
    std::fill(ringL.begin(), ringL.end(), 0.0f);
    std::fill(ringR.begin(), ringR.end(), 0.0f);
    std::fill(dryL.begin(), dryL.end(), 0.0f);
    std::fill(dryR.begin(), dryR.end(), 0.0f);
    written = 0;
    freezeEnd = 0;
    frozenOffset = 0.0;
    wasFrozen = false;
    dryPos = 0;
    gain = gainTarget;
    mix = mixTarget;
    resetVocoder();
}

void Engine::resetVocoder() {
    std::fill(prevRe.begin(), prevRe.end(), 0.0f);
    std::fill(prevIm.begin(), prevIm.end(), 0.0f);
    std::fill(olaL.begin(), olaL.end(), 0.0f);
    std::fill(olaR.begin(), olaR.end(), 0.0f);
    olaPos = 0;
    hopCounter = 0;
}

void Engine::setTranspose(double semitones) {
    semitones = std::clamp(semitones, -24.0, 24.0);
    tr = std::pow(2.0, semitones / 12.0);
}

void Engine::setMode(Mode m) {
    if (m == Mode::Effect && !hasEffectBuffers) return;   // voix de sampler : lecteur uniquement
    if (m == currentMode) return;
    currentMode = m;
    resetVocoder();
    gate = 0.0f;
    if (m == Mode::Effect) {
        playing = false;
        setGate(1.0f);
    } else {
        setGate(0.0f);
    }
}

void Engine::setGate(float target) {
    gateTarget = target;
    const float samples = (float) (0.030 * sr);   // [line~] 30 ms
    gateStep = (gateTarget - gate) / std::max(1.0f, samples);
}

void Engine::setPlayerBuffer(std::shared_ptr<const PlayerBuffer> b) {
    player = std::move(b);
    playing = false;
    stopCountdown = -1;
    start = PAD;
    end = PAD + (double) (player ? player->length : 0);
    loc = PAD;
    // En mode effet, le volume de sortie ne depend pas du lecteur : on n'y touche pas.
    if (currentMode == Mode::Player) {
        gate = 0.0f;
        gateTarget = 0.0f;
        gateStep = 0.0f;
        resetVocoder();
    }
}

// ------------------------------------------------------------------ transport (pv-play, ...)

void Engine::play() {
    if (!player || currentMode != Mode::Player) return;
    if (speed >= 0 && loc >= end) loc = start;
    else if (speed < 0 && loc <= start) loc = end;
    playing = true;
    stopCountdown = -1;
    setGate(1.0f);
}

void Engine::pause() {
    playing = false;
    if (currentMode == Mode::Player) setGate(0.0f);
}

void Engine::stop() {
    playing = false;
    if (currentMode == Mode::Player) {
        setGate(0.0f);
        stopCountdown = (int) (0.040 * sr);   // [delay 40] puis pv_loc = pv_start
    } else {
        loc = start;
    }
}

void Engine::seek(double fraction) {
    fraction = std::clamp(fraction, 0.0, 1.0);
    gliding = false;
    if (currentMode == Mode::Effect) {
        if (freeze) frozenOffset = (1.0 - fraction) * maxBackSamples();
        return;
    }
    loc = start + fraction * (end - start);
}

double Engine::position() const {
    if (currentMode == Mode::Effect) {
        if (!freeze) return 1.0;
        const double mb = maxBackSamples();
        return mb > 0 ? std::clamp(1.0 - frozenOffset / mb, 0.0, 1.0) : 1.0;
    }
    const double len = std::max(1.0, end - start);
    return std::clamp((loc - start) / len, 0.0, 1.0);
}

double Engine::maxBackSamples() const {
    // les fenetres lisent jusqu'a 3N * (transposition max) echantillons avant la position
    const double margin = EFFECT_LAG + 0.75 * N * MAX_TRANSPO + 64.0;
    double mb = std::min(8.0 * sr, (double) RING - margin);
    mb = std::min(mb, (double) freezeEnd - margin);   // pas plus que ce qui a ete enregistre
    return std::max(0.0, mb);
}

void Engine::setScrubTime(double ms) {
    ms = std::clamp(ms, 0.0, 1000.0);
    if (std::fabs(ms - scrubMs) < 1e-9 && scrubCoef > 0.0 && sr == lastScrubSr) return;
    scrubMs = ms;
    lastScrubSr = sr;
    // la position est mise a jour une fois par trame (H echantillons) : constante de temps exponentielle
    scrubCoef = ms <= 0.0 ? 1.0 : 1.0 - std::exp(-(double) H / (ms * 0.001 * sr));
}

void Engine::setScrub(bool active, double fraction) {
    scrubFrac = std::clamp(fraction, 0.0, 1.0);
    if (active && !scrubbing) {
        scrubbing = true;
        gliding = true;
        stopCountdown = -1;
        if (currentMode == Mode::Player && player) setGate(1.0f);   // audible meme en pause
    } else if (!active && scrubbing) {
        scrubbing = false;   // le glissement se termine en douceur (voir scrubStep)
        if (currentMode == Mode::Player && player && !playing) {
            // en pause : le son s'arrete au relachement, la position est celle du curseur
            gliding = false;
            loc = start + scrubFrac * (end - start);
            setGate(0.0f);
        }
    }
}

// Appele a chaque trame : rapproche la position de la cible du scrub.
void Engine::scrubStep() {
    if (!gliding) return;
    if (currentMode == Mode::Effect) {
        if (!freeze) { gliding = false; return; }
        const double target = (1.0 - scrubFrac) * maxBackSamples();
        frozenOffset += (target - frozenOffset) * scrubCoef;
        if (!scrubbing && std::fabs(target - frozenOffset) < 2.0 * H) { frozenOffset = target; gliding = false; }
    } else if (player) {
        const double target = start + scrubFrac * (end - start);
        loc += (target - loc) * scrubCoef;
        if (!scrubbing && std::fabs(target - loc) < 2.0 * H) {
            loc = target;
            gliding = false;
            if (!playing) setGate(0.0f);
        }
    } else {
        gliding = false;
    }
}

// ------------------------------------------------------------------ lecture des fenetres

// Equivalent exact de [tabread4~] (Pd 0.51) avec index (float) + onset (double).
static inline float tabread4(const float* buf, int64_t npoints, double findex) {
    const int64_t maxindex = npoints - 3;
    if (maxindex < 1) return 0.0f;
    int64_t index = (int64_t) findex;
    float frac;
    if (index < 1) { index = 1; frac = 0.0f; }
    else if (index > maxindex) { index = maxindex; frac = 1.0f; }
    else frac = (float) (findex - (double) index);
    const float* wp = buf + index;
    const float a = wp[-1], b = wp[0], c = wp[1], d = wp[2];
    const float cminusb = c - b;
    return b + frac * (cminusb - 0.1666667f * (1.0f - frac) *
                       ((d - a - 3.0f * cminusb) * frac + (d + 2.0f * a - 3.0f * b)));
}

void Engine::readWindow(const float* buf, int64_t bufLen, double startPos, float trf, float* out) const {
    // comme le patch : onset = partie entiere, fraction ajoutee au signal de rampe
    const double onset = (double) (int64_t) startPos;
    const float frac = (float) (startPos - onset);
    for (int m = 0; m < N; ++m) {
        const float idx = (float) m * trf + frac;
        out[m] = tabread4(buf, bufLen, (double) idx + onset) * hann[m];
    }
}

void Engine::readWindowCircular(const float* buf, double startPos, float trf, float* out) const {
    const double fl = std::floor(startPos);
    int64_t base = (int64_t) fl % RING;
    if (base < 1) base += RING;                 // le miroir garantit une zone contiguë
    const float frac = (float) (startPos - fl);
    const float* p = buf + base;
    for (int m = 0; m < N; ++m) {
        const float idx = (float) m * trf + frac;
        const int i = (int) idx;
        const float f = idx - (float) i;
        const float* wp = p + i;
        const float a = wp[-1], b = wp[0], c = wp[1], d = wp[2];
        const float cminusb = c - b;
        out[m] = (b + f * (cminusb - 0.1666667f * (1.0f - f) *
                           ((d - a - 3.0f * cminusb) * f + (d + 2.0f * a - 3.0f * b)))) * hann[m];
    }
}

// ------------------------------------------------------------------ FFT

void Engine::fft(float* re, float* im, bool inverse) {
    const auto& t = twiddles();
    for (int i = 0; i < N; ++i) {
        const int j = t.bitrev[i];
        if (j > i) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= N; len <<= 1) {
        const int half = len >> 1, step = N / len;
        for (int i = 0; i < N; i += len) {
            for (int k = 0; k < half; ++k) {
                const float c = t.cosT[k * step];
                const float s = inverse ? t.sinT[k * step] : -t.sinT[k * step];
                const int a = i + k, b = a + half;
                const float xr = re[b] * c - im[b] * s;
                const float xi = re[b] * s + im[b] * c;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr;        im[a] += xi;
            }
        }
    }
}

float Engine::rsqrt(float x) {
#ifdef PVOC_PD_RSQRT
    // approximation de [rsqrt~] de Pd (pour comparer les sorties)
    static float expt[256], mant[1024];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; ++i) {
            union { float f; int32_t l; } u;
            u.l = (i ? (i == 255 ? 254 : i) : 1) << 23;
            expt[i] = (float) (1.0 / std::sqrt((double) u.f));
        }
        for (int i = 0; i < 1024; ++i) mant[i] = (float) (1.0 / std::sqrt(1.0 + i / 1024.0));
        init = true;
    }
    if (x < 0) return 0;
    union { float f; int32_t l; } u;
    u.f = x;
    const float g = expt[(u.l >> 23) & 0xff] * mant[(u.l >> 13) & 0x3ff];
    return (float) (1.5 * g - 0.5 * (double) g * g * g * x);   // Pd calcule en double
#else
    return x > 0.0f ? 1.0f / std::sqrt(x) : 0.0f;
#endif
}

// ------------------------------------------------------------------ une trame du vocodeur

void Engine::computeFrame(double frontStart, double backStart) {
    const float trf = (float) tr;
    if (currentMode == Mode::Player) {
        const auto* b = player.get();
        const int64_t len = (int64_t) b->left.size();
        readWindow(b->left.data(), len, frontStart, trf, wFL.data());
        readWindow(b->right.data(), len, frontStart, trf, wFR.data());
        readWindow(b->left.data(), len, backStart, trf, reB.data());
        readWindow(b->right.data(), len, backStart, trf, imB.data());
    } else {
        readWindowCircular(ringL.data(), frontStart, trf, wFL.data());
        readWindowCircular(ringR.data(), frontStart, trf, wFR.data());
        readWindowCircular(ringL.data(), backStart, trf, reB.data());
        readWindowCircular(ringR.data(), backStart, trf, imB.data());
    }
    for (int m = 0; m < N; ++m) { wB[m] = reB[m] + imB[m]; }

    // FFT de L et R en une seule transformee complexe (L + iR), puis FFT de B
    for (int m = 0; m < N; ++m) { reL[m] = wFL[m]; imL[m] = wFR[m]; reB[m] = wB[m]; imB[m] = 0.0f; }
    fft(reL.data(), imL.data(), false);
    fft(reB.data(), imB.data(), false);

    const int K = N / 2;
    // separation des spectres de L et R ; reR/imR = R, reL/imL = L (bins 0..K)
    for (int k = 0; k <= K; ++k) {
        const int nk = (N - k) & (N - 1);
        const float zr = reL[k], zi = imL[k], cr = reL[nk], ci = -imL[nk];
        wFL[k] = 0.5f * (zr + cr);  wFR[k] = 0.5f * (zi + ci);        // L
        wB[k]  = 0.5f * (zi - ci);  reR[k] = -0.5f * (zr - cr);        // R = (Z - conj Z')/2i
    }
    for (int k = 0; k <= K; ++k) {
        reL[k] = wFL[k]; imL[k] = wFR[k];
        imR[k] = reR[k]; reR[k] = wB[k];
    }

    // Q = conj(B) * prev / |prev|
    for (int k = 0; k <= K; ++k) {
        const float pr = prevRe[k], pi = prevIm[k];
        const float n1 = rsqrt(pr * pr + pi * pi + 1e-20f);
        const float pnr = pr * n1, pni = pi * n1;
        const float br = reB[k], bi = imB[k];
        qRe[k] = br * pnr + bi * pni;
        qIm[k] = br * pni - bi * pnr;
    }
    // verrouillage de phase : ajoute les bandes voisines ([lrshift~ 1] et [lrshift~ -1])
    for (int k = 0; k <= K; ++k) {
        const float lr = (k > 0 ? qRe[k - 1] : 0.0f) + (k < K ? qRe[k + 1] : 0.0f);
        const float li = (k > 0 ? qIm[k - 1] : 0.0f) + (k < K ? qIm[k + 1] : 0.0f);
        uRe[k] = qRe[k] + lock * lr + 1e-15f;
        uIm[k] = qIm[k] + lock * li;
    }
    for (int k = 0; k <= K; ++k) {
        const float n2 = rsqrt(uRe[k] * uRe[k] + uIm[k] * uIm[k]);
        uRe[k] *= n2; uIm[k] *= n2;
    }
    // sortie : F * U ; la phase memorisee est celle de (L + R) * U
    for (int k = 0; k <= K; ++k) {
        const float ur = uRe[k], ui = uIm[k];
        const float fr = reL[k] + reR[k], fi = imL[k] + imR[k];
        prevRe[k] = fr * ur - fi * ui;
        prevIm[k] = fr * ui + fi * ur;
        const float lr = reL[k] * ur - imL[k] * ui, li = reL[k] * ui + imL[k] * ur;
        const float rr = reR[k] * ur - imR[k] * ui, ri = reR[k] * ui + imR[k] * ur;
        reL[k] = lr; imL[k] = li; reR[k] = rr; imR[k] = ri;
    }
    // IFFT des deux canaux en une transformee : spectre de (L + iR), symetrise
    for (int k = 0; k <= K; ++k) {
        const float lr = reL[k], li = (k == 0 || k == K) ? 0.0f : imL[k];
        const float rr = reR[k], ri = (k == 0 || k == K) ? 0.0f : imR[k];
        // Z[k] = L[k] + i R[k]
        wFL[k] = lr - ri; wFR[k] = li + rr;
        if (k > 0 && k < K) {
            // Z[N-k] = conj(L[k]) + i conj(R[k])
            wFL[N - k] = lr + ri; wFR[N - k] = -li + rr;
        }
    }
    fft(wFL.data(), wFR.data(), true);
    const float scale = 2.0f / (3.0f * N);
    for (int m = 0; m < N; ++m) {
        const int p = (olaPos + m) & (N - 1);
        olaL[p] += wFL[m] * hann[m] * scale;
        olaR[p] += wFR[m] * hann[m] * scale;
    }
}

void Engine::playerAdvance() {
    loc += (playing ? 1.0 : 0.0) * speed * H;
    if (playing && speed > 0 && loc >= end) {
        if (loop) loc -= (end - start);
        else { playing = false; loc = end; endFlag = true; setGate(0.0f); }
    } else if (playing && speed < 0 && loc <= start) {
        if (loop) loc += (end - start);
        else { playing = false; loc = start; endFlag = true; setGate(0.0f); }
    }
}

// ------------------------------------------------------------------ traitement

void Engine::process(const float* inL, const float* inR, float* outL, float* outR, int n) {
    const bool effect = currentMode == Mode::Effect;
    if (effect && gateTarget < 1.0f) setGate(1.0f);   // securite : en mode effet la sortie est toujours ouverte
    for (int i = 0; i < n; ++i) {
        float dl = 0.0f, dr = 0.0f;
        if (effect) {
            const float xl = inL ? inL[i] : 0.0f;
            const float xr = inR ? inR[i] : (inL ? inL[i] : 0.0f);
            if (!freeze) {
                const int64_t w = written % RING;
                ringL[w] = ringL[w + RING] = xl;
                ringR[w] = ringR[w + RING] = xr;
                if (w < 16) { ringL[w + 2 * RING] = xl; ringR[w + 2 * RING] = xr; }
                ++written;
            }
            dl = dryL[dryPos]; dr = dryR[dryPos];
            dryL[dryPos] = xl; dryR[dryPos] = xr;
            if (++dryPos >= EFFECT_LATENCY) dryPos = 0;
        } else if (stopCountdown >= 0) {
            if (stopCountdown-- == 0) loc = start;
        }

        if (hopCounter == 0) {
            if (effect) {
                if (freeze && !wasFrozen) { freezeEnd = written; frozenOffset = 0.0; }
                wasFrozen = freeze;
                scrubStep();
                if (!freeze) loc = (double) written - EFFECT_LAG;
                else loc = (double) freezeEnd - EFFECT_LAG - frozenOffset;
                const double fs = loc - (N / 2) * tr;
                computeFrame(fs, fs - (N / 4) * tr);
            } else if (player) {
                scrubStep();
                const double fs = loc - (N / 2) * tr;
                computeFrame(fs, fs - (N / 4) * tr);
                if (!gliding) playerAdvance();   // pendant le scrub, c'est le curseur qui mene
            }
        }
        if (++hopCounter >= H) hopCounter = 0;

        const float wl = olaL[olaPos], wr = olaR[olaPos];
        olaL[olaPos] = 0.0f; olaR[olaPos] = 0.0f;
        olaPos = (olaPos + 1) & (N - 1);

        // rampes
        if (gateStep > 0.0f) {
            gate += gateStep;
            if (gate >= gateTarget) { gate = gateTarget; gateStep = 0.0f; }
        } else if (gateStep < 0.0f) {
            gate += gateStep;
            if (gate <= gateTarget) { gate = gateTarget; gateStep = 0.0f; }
        }
        gain += (gainTarget - gain) * rampCoef;
        mix += (mixTarget - mix) * rampCoef;

        if (effect) {
            outL[i] = (dl * (1.0f - mix) + wl * gate * mix) * gain;
            outR[i] = (dr * (1.0f - mix) + wr * gate * mix) * gain;
        } else {
            outL[i] = wl * gate * gain;
            outR[i] = wr * gate * gain;
        }
    }
}

} // namespace pvoc
