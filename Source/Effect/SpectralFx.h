// Moteur de l'Effet PVoc : traitement spectral en direct (STFT 2048 points, recouvrement 4).
// Gel, pitch-shift a verrouillage de phase par pics (Laroche-Dolson), voix d'harmoniseur,
// decalage de frequence, robotisation / vocodeur, chuchotement, flou spectral, porte spectrale,
// cross-synthese avec un modulateur (sidechain ou fichier).
// C++ pur, sans allocation dans process().
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

#include "../PvocEngine.h"   // PlayerBuffer (fichier modulateur)

namespace pvoc {

class SpectralFx {
public:
    static constexpr int N = 2048;
    static constexpr int H = N / 4;
    static constexpr int K = N / 2 + 1;
    static constexpr int LATENCY = N - 1;
    static constexpr int MAX_VOICES = 16;
    static constexpr int SPEC_BANDS = 96;

    struct Params {
        bool freeze = false;
        float pitchSemis = 0.0f;      // voix principale
        float fshiftHz = 0.0f;        // decalage de frequence (toutes les voix)
        bool keepMain = true;         // voix principale gardee quand des notes MIDI jouent
        float robot = 0.0f;           // 0..1
        float robotHz = 110.0f;       // hauteur du robot sans clavier
        float whisper = 0.0f;         // 0..1
        float blur = 0.0f;            // 0..1
        float gateDb = -90.0f;        // porte spectrale, relative au maximum de la trame (-90 = inactive)
        float cross = 0.0f;           // 0..1
        float crossSmooth = 0.4f;     // 0..1 (0 = remplacement des amplitudes)
        int crossSource = 0;          // 0 sidechain, 1 fichier
        float attackMs = 10.0f, releaseMs = 200.0f;
        float mix = 1.0f, gain = 1.0f;
    };

    /** Etat d'une voix commandee par le clavier (fourni a chaque bloc). */
    struct VoiceIn {
        bool gate = false;      // note tenue
        float semis = 0.0f;     // transposition de l'harmoniseur
        float robotHz = 0.0f;   // hauteur du robot pour cette note
        float level = 1.0f;     // velocite
    };

    SpectralFx();
    void prepare(double sampleRate);
    double sampleRate() const { return sr; }
    void setParams(const Params& p) { prm = p; }
    void setVoices(const VoiceIn* v, int count, bool harmonizer, bool robotKeys);
    void setModulatorFile(std::shared_ptr<const PlayerBuffer> b) { modFile = std::move(b); }
    const std::shared_ptr<const PlayerBuffer>& modulatorFile() const { return modFile; }

    /** sc : sidechain (peut etre nul). Traitement en place autorise. */
    void process(const float* inL, const float* inR, const float* scL, const float* scR,
                 float* outL, float* outR, int n);

    int activeVoices() const { return activeCount; }
    // pour l'affichage (lecture depuis l'interface, valeurs indicatives)
    std::atomic<float> specIn[SPEC_BANDS], specOut[SPEC_BANDS];
    std::atomic<float> modLevel { 0.0f };

private:
    struct Voice {
        float env = 0.0f;
        bool gate = false;
        float semis = 0.0f, robotHz = 0.0f, level = 1.0f;
        std::vector<float> psi[2];           // phases de synthese par canal
        std::vector<double> robPhase[2];     // phases des harmoniques du robot
        bool fresh = true;
    };

    void computeFrame();
    void analyse(const float* ringL, const float* ringR);
    void remapAdd(int c, Voice& v, double ratio, double shiftBins, float gain, bool exact);
    void robotAdd(int c, Voice& v, double f0Hz, double shiftBins, float gain);
    void fft(float* re, float* im, bool inverse) const;
    static void boxSmooth(const float* in, float* out, int w, float* tmp);
    void updateSpectrumDisplay();

    double sr = 48000.0;
    Params prm;
    std::shared_ptr<const PlayerBuffer> modFile;
    int64_t filePos = 0;

    // FFT
    std::vector<float> cosT, sinT;
    std::vector<int> bitrev;
    std::vector<float> hann;

    // tampons
    std::vector<float> ringL, ringR, ringSc;   // N derniers echantillons
    int ringPos = 0;
    std::vector<float> dryL, dryR;
    int dryPos = 0;
    std::vector<float> olaL, olaR;
    int olaPos = 0;
    int hopCounter = 0;

    // analyse par canal
    std::vector<float> fre, fim;                      // travail FFT
    std::vector<float> mag[2], ph[2], omega[2], phPrev[2];
    std::vector<float> magF[2], phF[2], omegaF[2];    // trame gelee
    std::vector<float> magBlur[2], omegaHold[2], phHold[2];
    std::vector<float> modMag, envA, envB, tmpS;
    std::vector<float> yRe[2], yIm[2], yRobRe[2], yRobIm[2];
    std::vector<float> psiNew, bestMag;
    std::vector<int> peaks;
    bool frozen = false;

    Voice mainVoice;
    std::vector<Voice> voices;   // MAX_VOICES
    bool harmonizerKeys = true, robotFromKeys = false;
    int activeCount = 0;

    std::minstd_rand rng { 12345 };
    float mixCur = 1.0f, gainCur = 1.0f, smoothK = 0.001f;
};

} // namespace pvoc
