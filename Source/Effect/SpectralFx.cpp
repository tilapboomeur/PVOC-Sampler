#include "SpectralFx.h"

#include <algorithm>
#include <cmath>

namespace pvoc {

namespace {
constexpr double PI = 3.14159265358979323846;
constexpr double TWO_PI = 2.0 * PI;
constexpr float HALF_PI = (float) (PI / 2.0);   // avance de phase par trame et par bin (2 pi H / N)
inline float wrap(float x) { return (float) std::remainder((double) x, TWO_PI); }
} // namespace

SpectralFx::SpectralFx() {
    cosT.resize(N / 2);
    sinT.resize(N / 2);
    for (int k = 0; k < N / 2; ++k) {
        cosT[(size_t) k] = (float) std::cos(TWO_PI * k / N);
        sinT[(size_t) k] = (float) std::sin(TWO_PI * k / N);
    }
    bitrev.resize(N);
    int bits = 0;
    while ((1 << bits) < N) ++bits;
    for (int i = 0; i < N; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b) r |= ((i >> b) & 1) << (bits - 1 - b);
        bitrev[(size_t) i] = r;
    }
    hann.resize(N);
    for (int i = 0; i < N; ++i) hann[(size_t) i] = (float) (0.5 - 0.5 * std::cos(TWO_PI * i / N));

    ringL.assign(N, 0.0f);
    ringR.assign(N, 0.0f);
    ringSc.assign(N, 0.0f);
    dryL.assign(LATENCY, 0.0f);
    dryR.assign(LATENCY, 0.0f);
    olaL.assign(N, 0.0f);
    olaR.assign(N, 0.0f);
    fre.assign(N, 0.0f);
    fim.assign(N, 0.0f);
    for (int c = 0; c < 2; ++c) {
        for (auto* v : { &mag[c], &ph[c], &omega[c], &phPrev[c], &magF[c], &phF[c], &omegaF[c], &magBlur[c], &omegaHold[c], &phHold[c],
                         &yRe[c], &yIm[c], &yRobRe[c], &yRobIm[c] })
            v->assign(K, 0.0f);
    }
    for (auto* v : { &modMag, &envA, &envB, &tmpS, &psiNew, &bestMag }) v->assign(K, 0.0f);
    peaks.reserve(K);

    auto initVoice = [](Voice& v) {
        for (int c = 0; c < 2; ++c) {
            v.psi[c].assign(K, 0.0f);
            v.robPhase[c].assign(4096, 0.0);
        }
    };
    initVoice(mainVoice);
    mainVoice.env = 1.0f;
    voices.resize(MAX_VOICES);
    for (auto& v : voices) initVoice(v);
    for (int b = 0; b < SPEC_BANDS; ++b) { specIn[b] = -120.0f; specOut[b] = -120.0f; }
    prepare(48000.0);
}

void SpectralFx::prepare(double sampleRate) {
    sr = sampleRate > 0 ? sampleRate : 48000.0;
    std::fill(ringL.begin(), ringL.end(), 0.0f);
    std::fill(ringR.begin(), ringR.end(), 0.0f);
    std::fill(ringSc.begin(), ringSc.end(), 0.0f);
    std::fill(dryL.begin(), dryL.end(), 0.0f);
    std::fill(dryR.begin(), dryR.end(), 0.0f);
    std::fill(olaL.begin(), olaL.end(), 0.0f);
    std::fill(olaR.begin(), olaR.end(), 0.0f);
    for (int c = 0; c < 2; ++c) {
        std::fill(phPrev[c].begin(), phPrev[c].end(), 0.0f);
        std::fill(magBlur[c].begin(), magBlur[c].end(), 0.0f);
    }
    ringPos = dryPos = olaPos = hopCounter = 0;
    frozen = false;
    filePos = 0;
    mainVoice.fresh = true;
    for (auto& v : voices) { v.env = 0.0f; v.gate = false; v.fresh = true; }
    mixCur = prm.mix;
    gainCur = prm.gain;
    smoothK = (float) (1.0 - std::exp(-1.0 / (0.010 * sr)));
}

void SpectralFx::setVoices(const VoiceIn* v, int count, bool harmonizer, bool robotKeys) {
    harmonizerKeys = harmonizer;
    robotFromKeys = robotKeys;
    for (int i = 0; i < MAX_VOICES; ++i) {
        auto& d = voices[(size_t) i];
        const bool g = i < count && v[i].gate;
        if (g && !d.gate && d.env <= 0.0f) d.fresh = true;   // nouvelle note : phases reparties de l'analyse
        d.gate = g;
        if (i < count && (g || d.env > 0.0f)) {
            if (g) {   // une note relachee garde sa hauteur pendant le relachement
                d.semis = v[i].semis;
                d.robotHz = v[i].robotHz;
                d.level = v[i].level;
            }
        }
    }
}

// ------------------------------------------------------------------ FFT

void SpectralFx::fft(float* re, float* im, bool inverse) const {
    for (int i = 0; i < N; ++i) {
        const int j = bitrev[(size_t) i];
        if (j > i) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= N; len <<= 1) {
        const int half = len >> 1, step = N / len;
        for (int i = 0; i < N; i += len) {
            for (int k = 0; k < half; ++k) {
                const float c = cosT[(size_t) (k * step)];
                const float s = inverse ? sinT[(size_t) (k * step)] : -sinT[(size_t) (k * step)];
                const int a = i + k, b = a + half;
                const float xr = re[b] * c - im[b] * s;
                const float xi = re[b] * s + im[b] * c;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr;        im[a] += xi;
            }
        }
    }
}

void SpectralFx::boxSmooth(const float* in, float* out, int w, float* tmp) {
    // moyenne glissante de largeur 2w+1 (somme cumulee)
    double acc = 0.0;
    for (int k = 0; k < K; ++k) { acc += in[k]; tmp[k] = (float) acc; }
    for (int k = 0; k < K; ++k) {
        const int a = std::max(0, k - w), b = std::min(K - 1, k + w);
        const double s = tmp[b] - (a > 0 ? tmp[a - 1] : 0.0f);
        out[k] = (float) (s / (b - a + 1));
    }
}

// ------------------------------------------------------------------ une trame

void SpectralFx::analyse(const float* rL, const float* rR) {
    for (int m = 0; m < N; ++m) {
        const int idx = (ringPos + m) & (N - 1);   // du plus ancien au plus recent
        fre[(size_t) m] = rL[idx] * hann[(size_t) m];
        fim[(size_t) m] = rR[idx] * hann[(size_t) m];
    }
    fft(fre.data(), fim.data(), false);
    // separation des spectres des deux canaux (FFT complexe de L + iR)
    for (int k = 0; k < K; ++k) {
        const int nk = (N - k) & (N - 1);
        const float zr = fre[(size_t) k], zi = fim[(size_t) k], cr = fre[(size_t) nk], ci = -fim[(size_t) nk];
        const float lr = 0.5f * (zr + cr), li = 0.5f * (zi + ci);
        const float rr = 0.5f * (zi - ci), ri = -0.5f * (zr - cr);
        const float re2[2] = { lr, rr }, im2[2] = { li, ri };
        for (int c = 0; c < 2; ++c) {
            mag[c][(size_t) k] = std::sqrt(re2[c] * re2[c] + im2[c] * im2[c]);
            const float p = std::atan2(im2[c], re2[c]);
            ph[c][(size_t) k] = p;
            // frequence vraie (en bins) par la difference de phase entre deux trames
            const float dev = wrap(p - phPrev[c][(size_t) k] - HALF_PI * (float) k);
            omega[c][(size_t) k] = (float) k + dev / HALF_PI;
            phPrev[c][(size_t) k] = p;
        }
    }
}

void SpectralFx::remapAdd(int c, Voice& v, double ratio, double shiftBins, float gain, bool exact) {
    auto& M = mag[c];
    auto& P = ph[c];
    auto& W = omega[c];
    auto& psi = v.psi[c];
    auto& Yr = yRe[c];
    auto& Yi = yIm[c];

    if (exact) {   // aucun changement de hauteur : phases d'origine, reconstruction exacte
        for (int k = 0; k < K; ++k) {
            const float m = M[(size_t) k] * gain;
            Yr[(size_t) k] += m * std::cos(P[(size_t) k]);
            Yi[(size_t) k] += m * std::sin(P[(size_t) k]);
            psi[(size_t) k] = P[(size_t) k];
        }
        return;
    }

    // pics du spectre
    float mx = 0.0f;
    for (int k = 0; k < K; ++k) mx = std::max(mx, M[(size_t) k]);
    peaks.clear();
    const float floor = mx * 1e-5f + 1e-12f;
    for (int k = 2; k < K - 2; ++k) {   // maximum sur +/- 2 bins : les lobes secondaires ne sont pas des pics
        const float m = M[(size_t) k];
        if (m > floor && m > M[(size_t) k - 1] && m >= M[(size_t) k + 1] && m > M[(size_t) k - 2] && m >= M[(size_t) k + 2])
            peaks.push_back(k);
    }
    for (int j = 0; j < K; ++j) {
        psiNew[(size_t) j] = wrap(psi[(size_t) j] + HALF_PI * (float) j);
        bestMag[(size_t) j] = 0.0f;
    }
    const int np = (int) peaks.size();
    for (int i = 0; i < np; ++i) {
        const int p = peaks[(size_t) i];
        const double fp = W[(size_t) p] * ratio + shiftBins;   // nouvelle frequence du pic, en bins
        if (fp <= 0.5 || fp >= K - 1.5) continue;
        const int jp = (int) std::lround(fp);
        // phase du pic : prolongement de la phase synthetisee a la trame precedente
        const float base = psi[(size_t) jp] + HALF_PI * (float) fp;
        // region du pic : jusqu'au creux le plus bas avec chaque pic voisin
        auto valley = [&](int lo, int hi) {
            int v = lo;
            for (int k = lo; k <= hi; ++k) if (M[(size_t) k] < M[(size_t) v]) v = k;
            return v;
        };
        const int a = i == 0 ? 0 : valley(peaks[(size_t) i - 1] + 1, p) ;
        const int b = i == np - 1 ? K - 1 : valley(p, peaks[(size_t) i + 1] - 1) - 1;
        // la region du pic est deplacee de (frequence vraie transposee - frequence vraie) bins,
        // avec une precision fractionnaire : la forme du lobe suit exactement la nouvelle frequence
        const double shift = fp - W[(size_t) p];
        const int j0 = std::max(0, (int) std::ceil(a + shift)), j1 = std::min(K - 1, (int) std::floor(b + shift));
        for (int j = j0; j <= j1; ++j) {
            const double ks = j - shift;              // position source (fractionnaire)
            // interpolation parabolique du logarithme de l'amplitude : suit la forme du lobe de la fenetre
            const int kc = std::max(1, std::min(K - 2, (int) std::lround(ks)));
            const double x = std::max(-1.0, std::min(1.0, ks - kc));
            const double lm = std::log(M[(size_t) kc - 1] + 1e-12), l0 = std::log(M[(size_t) kc] + 1e-12),
                         lp = std::log(M[(size_t) kc + 1] + 1e-12);
            const float mk = (float) std::exp(l0 + 0.5 * x * (lp - lm) + 0.5 * x * x * (lp - 2.0 * l0 + lm));
            // phases : decalage entier (jp - p), qui garde l'alternance des phases autour du pic
            const int kn = std::max(a, std::min(b, j - (jp - p)));
            const float phase = base + (P[(size_t) kn] - P[(size_t) p]);   // verrouillage autour du pic
            const float m = mk * gain;
            Yr[(size_t) j] += m * std::cos(phase);
            Yi[(size_t) j] += m * std::sin(phase);
            if (mk > bestMag[(size_t) j]) {
                bestMag[(size_t) j] = mk;
                psiNew[(size_t) j] = wrap(phase);
            }
        }
    }
    std::copy(psiNew.begin(), psiNew.end(), psi.begin());
}

void SpectralFx::robotAdd(int c, Voice& v, double f0Hz, double shiftBins, float gain) {
    const double binsPerHarm = f0Hz * N / sr;
    if (binsPerHarm < 0.5) return;
    const float norm = (float) std::sqrt(std::max(1.0, binsPerHarm));   // energie conservee
    auto& rp = v.robPhase[c];
    const auto& env = c == 0 ? envA : envB;
    for (int h = 1; h < (int) rp.size(); ++h) {
        const double b = h * binsPerHarm + shiftBins;
        if (b >= K - 1) break;
        if (b <= 0.5) continue;
        const int j0 = (int) b;
        const float fr = (float) (b - j0);
        const float amp = (env[(size_t) j0] + (env[(size_t) j0 + 1] - env[(size_t) j0]) * fr) * norm * gain;
        rp[(size_t) h] = std::remainder(rp[(size_t) h] + (PI / 2.0) * b, TWO_PI);
        const int j = (int) std::lround(b);
        yRobRe[c][(size_t) j] += amp * (float) std::cos(rp[(size_t) h]);
        yRobIm[c][(size_t) j] += amp * (float) std::sin(rp[(size_t) h]);
    }
}

void SpectralFx::computeFrame() {
    analyse(ringL.data(), ringR.data());

    // entree pour l'affichage (avant traitement)
    {
        const double binHz = sr / N;
        for (int b = 0; b < SPEC_BANDS; ++b) {
            const double f0 = 30.0 * std::pow(20000.0 / 30.0, (double) b / SPEC_BANDS);
            const double f1 = 30.0 * std::pow(20000.0 / 30.0, (double) (b + 1) / SPEC_BANDS);
            int k0 = std::max(1, (int) (f0 / binHz)), k1 = std::min(K - 1, std::max(k0, (int) (f1 / binHz)));
            float m = 0.0f;
            for (int k = k0; k <= k1; ++k) m = std::max(m, 0.5f * (mag[0][(size_t) k] + mag[1][(size_t) k]));
            specIn[b] = 20.0f * std::log10(m * 4.0f / N + 1e-9f);
        }
    }

    // modulateur de la cross-synthese
    if (prm.cross > 0.0f) {
        const auto file = modFile;
        const bool useFile = prm.crossSource == 1 && file != nullptr && file->length > 0;
        double e = 0.0;
        for (int m = 0; m < N; ++m) {
            float x;
            if (useFile) {
                const int64_t len = file->length;
                int64_t idx = (filePos - N + 1 + m) % len;
                if (idx < 0) idx += len;
                const size_t at = (size_t) (Engine::PAD + idx);
                x = 0.5f * (file->left[at] + file->right[at]);
            } else {
                x = prm.crossSource == 0 ? ringSc[(size_t) ((ringPos + m) & (N - 1))] : 0.0f;
            }
            e += (double) x * x;
            fre[(size_t) m] = x * hann[(size_t) m];
            fim[(size_t) m] = 0.0f;
        }
        modLevel = (float) std::sqrt(e / N);
        fft(fre.data(), fim.data(), false);
        for (int k = 0; k < K; ++k) modMag[(size_t) k] = std::sqrt(fre[(size_t) k] * fre[(size_t) k] + fim[(size_t) k] * fim[(size_t) k]);
    } else {
        modLevel = 0.0f;
    }

    // gel : la trame courante est gardee (amplitudes, phases, frequences vraies)
    if (prm.freeze && !frozen) {
        for (int c = 0; c < 2; ++c) { magF[c] = mag[c]; phF[c] = ph[c]; omegaF[c] = omega[c]; }
        frozen = true;
    } else if (!prm.freeze) {
        frozen = false;
    }
    if (frozen)
        for (int c = 0; c < 2; ++c) {
            std::copy(magF[c].begin(), magF[c].end(), mag[c].begin());
            std::copy(phF[c].begin(), phF[c].end(), ph[c].begin());
            std::copy(omegaF[c].begin(), omegaF[c].end(), omega[c].begin());
        }

    for (int c = 0; c < 2; ++c) {
        auto& M = mag[c];
        // flou spectral : moyenne des amplitudes dans le temps
        if (prm.blur > 0.0f) {
            const double tau = 0.01 * std::pow(1000.0, (double) std::min(1.0f, prm.blur));   // 10 ms a 10 s
            const float a = (float) std::exp(-(double) H / (tau * sr));
            for (int k = 0; k < K; ++k) {
                const float mb = a * magBlur[c][(size_t) k] + (1.0f - a) * M[(size_t) k];
                magBlur[c][(size_t) k] = mb;
                // composante qui s'eteint : on garde sa frequence et sa phase relative pour la traine
                if (M[(size_t) k] > 0.3f * mb) {
                    omegaHold[c][(size_t) k] = omega[c][(size_t) k];
                    phHold[c][(size_t) k] = ph[c][(size_t) k];
                } else {
                    omega[c][(size_t) k] = omegaHold[c][(size_t) k];
                    ph[c][(size_t) k] = phHold[c][(size_t) k];
                }
                M[(size_t) k] = mb;
            }
        } else {
            std::copy(M.begin(), M.end(), magBlur[c].begin());
        }
        // cross-synthese : enveloppe spectrale du modulateur imposee a l'entree
        if (prm.cross > 0.0f) {
            const int w = (int) std::lround(prm.crossSmooth * 40.0f);
            const float x = std::min(1.0f, prm.cross);
            if (w <= 0) {
                for (int k = 0; k < K; ++k) M[(size_t) k] += (modMag[(size_t) k] - M[(size_t) k]) * x;
            } else {
                boxSmooth(M.data(), envA.data(), w, tmpS.data());
                boxSmooth(modMag.data(), envB.data(), w, tmpS.data());
                float emx = 0.0f;
                for (int k = 0; k < K; ++k) emx = std::max(emx, envA[(size_t) k]);
                const float eps = emx * 1e-4f + 1e-9f;
                for (int k = 0; k < K; ++k) {
                    const float g = std::min(1000.0f, envB[(size_t) k] / (envA[(size_t) k] + eps));
                    const float target = M[(size_t) k] * g;
                    M[(size_t) k] += (target - M[(size_t) k]) * x;
                }
            }
        }
        // porte spectrale : les composantes faibles disparaissent (transition douce)
        if (prm.gateDb > -89.9f) {
            float mx = 0.0f;
            for (int k = 0; k < K; ++k) mx = std::max(mx, M[(size_t) k]);
            const float thr = mx * std::pow(10.0f, prm.gateDb / 20.0f) + 1e-12f;
            for (int k = 0; k < K; ++k) {
                const float r = M[(size_t) k] / thr;
                M[(size_t) k] *= std::min(1.0f, r * r);
            }
        }
        std::fill(yRe[c].begin(), yRe[c].end(), 0.0f);
        std::fill(yIm[c].begin(), yIm[c].end(), 0.0f);
        std::fill(yRobRe[c].begin(), yRobRe[c].end(), 0.0f);
        std::fill(yRobIm[c].begin(), yRobIm[c].end(), 0.0f);
    }

    // enveloppes des voix (une fois par trame)
    const float hopMs = (float) (1000.0 * H / sr);
    const float att = std::min(1.0f, hopMs / std::max(1.0f, prm.attackMs));
    const float rel = std::min(1.0f, hopMs / std::max(1.0f, prm.releaseMs));
    bool anyKey = false;
    activeCount = 0;
    for (auto& v : voices) {
        const float target = v.gate ? 1.0f : 0.0f;
        v.env = target > v.env ? std::min(target, v.env + att) : std::max(target, v.env - rel);
        anyKey |= v.gate;
        if (v.gate || v.env > 0.0f) ++activeCount;
    }
    const float mainTarget = (harmonizerKeys && anyKey && !prm.keepMain) ? 0.0f : 1.0f;
    mainVoice.env = mainTarget > mainVoice.env ? std::min(mainTarget, mainVoice.env + att) : std::max(mainTarget, mainVoice.env - rel);

    const double shiftBins = prm.fshiftHz * N / sr;
    const double r0 = std::pow(2.0, prm.pitchSemis / 12.0);
    const bool exactMain = std::abs(prm.pitchSemis) < 1e-4f && std::abs(prm.fshiftHz) < 1e-3f && !frozen && prm.blur <= 0.0f;

    for (int c = 0; c < 2; ++c) {
        if (mainVoice.fresh) std::copy(ph[c].begin(), ph[c].end(), mainVoice.psi[c].begin());
        if (mainVoice.env > 0.0f) remapAdd(c, mainVoice, r0, shiftBins, mainVoice.env, exactMain);
        else remapAdd(c, mainVoice, r0, shiftBins, 0.0f, exactMain);   // garde les phases a jour
        if (harmonizerKeys)
            for (auto& v : voices) {
                if (v.env <= 0.0f) continue;
                if (v.fresh) std::copy(ph[c].begin(), ph[c].end(), v.psi[c].begin());
                remapAdd(c, v, std::pow(2.0, v.semis / 12.0), shiftBins, v.env * v.level, false);
            }

        // robotisation / vocodeur : peigne d'harmoniques module par l'enveloppe du son
        if (prm.robot > 0.0f) {
            auto& env = c == 0 ? envA : envB;
            boxSmooth(mag[c].data(), env.data(), 3, tmpS.data());
            bool keys = false;
            if (robotFromKeys)
                for (auto& v : voices)
                    if (v.env > 0.0f && v.robotHz > 0.0f) {
                        robotAdd(c, v, v.robotHz, shiftBins, v.env * v.level);
                        keys = true;
                    }
            if (!keys) robotAdd(c, mainVoice, prm.robotHz, shiftBins, 1.0f);
            const float ro = std::min(1.0f, prm.robot);
            for (int k = 0; k < K; ++k) {
                yRe[c][(size_t) k] = yRe[c][(size_t) k] * (1.0f - ro) + yRobRe[c][(size_t) k] * ro;
                yIm[c][(size_t) k] = yIm[c][(size_t) k] * (1.0f - ro) + yRobIm[c][(size_t) k] * ro;
            }
        }
    }
    mainVoice.fresh = false;
    for (auto& v : voices) if (v.env > 0.0f) v.fresh = false;

    // chuchotement : phases aleatoires
    if (prm.whisper > 0.0f) {
        std::uniform_real_distribution<float> U(-(float) PI, (float) PI);
        for (int c = 0; c < 2; ++c)
            for (int k = 0; k < K; ++k) {
                const float t = prm.whisper * U(rng);
                const float cr = std::cos(t), si = std::sin(t);
                const float re = yRe[c][(size_t) k], im = yIm[c][(size_t) k];
                yRe[c][(size_t) k] = re * cr - im * si;
                yIm[c][(size_t) k] = re * si + im * cr;
            }
    }

    // sortie pour l'affichage
    {
        const double binHz = sr / N;
        for (int b = 0; b < SPEC_BANDS; ++b) {
            const double f0 = 30.0 * std::pow(20000.0 / 30.0, (double) b / SPEC_BANDS);
            const double f1 = 30.0 * std::pow(20000.0 / 30.0, (double) (b + 1) / SPEC_BANDS);
            int k0 = std::max(1, (int) (f0 / binHz)), k1 = std::min(K - 1, std::max(k0, (int) (f1 / binHz)));
            float m = 0.0f;
            for (int k = k0; k <= k1; ++k) {
                const float a = std::hypot(yRe[0][(size_t) k], yIm[0][(size_t) k]), b2 = std::hypot(yRe[1][(size_t) k], yIm[1][(size_t) k]);
                m = std::max(m, 0.5f * (a + b2));
            }
            specOut[b] = 20.0f * std::log10(m * 4.0f / N + 1e-9f);
        }
    }

    // synthese : une IFFT complexe pour les deux canaux (L + iR)
    for (int k = 0; k < K; ++k) {
        const bool edge = k == 0 || k == K - 1;
        const float lr = yRe[0][(size_t) k], li = edge ? 0.0f : yIm[0][(size_t) k];
        const float rr = yRe[1][(size_t) k], ri = edge ? 0.0f : yIm[1][(size_t) k];
        fre[(size_t) k] = lr - ri;
        fim[(size_t) k] = li + rr;
        if (!edge) {
            fre[(size_t) (N - k)] = lr + ri;
            fim[(size_t) (N - k)] = -li + rr;
        }
    }
    fft(fre.data(), fim.data(), true);
    const float scale = 2.0f / (3.0f * N);
    for (int m = 0; m < N; ++m) {
        const int p = (olaPos + m) & (N - 1);
        olaL[(size_t) p] += fre[(size_t) m] * hann[(size_t) m] * scale;
        olaR[(size_t) p] += fim[(size_t) m] * hann[(size_t) m] * scale;
    }
}

// ------------------------------------------------------------------ traitement

void SpectralFx::process(const float* inL, const float* inR, const float* scL, const float* scR,
                         float* outL, float* outR, int n) {
    for (int i = 0; i < n; ++i) {
        const float xl = inL ? inL[i] : 0.0f;
        const float xr = inR ? inR[i] : xl;
        ringL[(size_t) ringPos] = xl;
        ringR[(size_t) ringPos] = xr;
        float sc = 0.0f;
        if (scL != nullptr) sc = scR != nullptr ? 0.5f * (scL[i] + scR[i]) : scL[i];
        ringSc[(size_t) ringPos] = sc;
        ringPos = (ringPos + 1) & (N - 1);

        const float dl = dryL[(size_t) dryPos], dr = dryR[(size_t) dryPos];
        dryL[(size_t) dryPos] = xl;
        dryR[(size_t) dryPos] = xr;
        if (++dryPos >= LATENCY) dryPos = 0;

        if (++hopCounter >= H) {
            hopCounter = 0;
            computeFrame();
            filePos += H;
            if (modFile != nullptr && modFile->length > 0) filePos %= modFile->length;
        }

        const float wl = olaL[(size_t) olaPos], wr = olaR[(size_t) olaPos];
        olaL[(size_t) olaPos] = 0.0f;
        olaR[(size_t) olaPos] = 0.0f;
        olaPos = (olaPos + 1) & (N - 1);

        mixCur += (prm.mix - mixCur) * smoothK;
        gainCur += (prm.gain - gainCur) * smoothK;
        outL[i] = (dl * (1.0f - mixCur) + wl * mixCur) * gainCur;
        outR[i] = (dr * (1.0f - mixCur) + wr * mixCur) * gainCur;
    }
}

} // namespace pvoc
