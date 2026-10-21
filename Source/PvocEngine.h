// Moteur "Lecteur PVoc" : traduction en C++ du patch Pure Data pvoc-engine.pd
// (vocodeur de phase stereo, fenetre 2048, recouvrement 4, verrouillage de phase).
// Aucun appel systeme ni allocation dans process() : utilisable dans un thread audio.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace pvoc {

/** Son charge en mode lecteur : deja a la frequence du moteur, avec PAD zeros avant et apres. */
struct PlayerBuffer {
    std::vector<float> left, right;   // taille = length + 2 * PAD (+ marge)
    int64_t length = 0;               // nombre d'echantillons utiles
};

class Engine {
public:
    static constexpr int N = 2048;              // taille de fenetre
    static constexpr int H = N / 4;             // pas (recouvrement 4)
    static constexpr int PAD = 8192;            // zeros autour du son (comme dans le patch)
    static constexpr double MAX_TRANSPO = 4.0;  // +24 demi-tons
    static constexpr int EFFECT_LAG = N * 2 + 4;            // N * MAX_TRANSPO / 2 + garde
    static constexpr int EFFECT_LATENCY = EFFECT_LAG + N / 2 - 1;

    enum class Mode { Player = 0, Effect = 1 };

    /** withEffectBuffers = false : pas de tampon du mode effet (~8 Mo economises), pour les voix du sampler. */
    explicit Engine(bool withEffectBuffers = true);

    /** A appeler hors du thread audio. */
    void prepare(double sampleRate);
    double sampleRate() const { return sr; }

    /** Change le son du lecteur (meme thread que process, ou sous verrou). */
    void setPlayerBuffer(std::shared_ptr<const PlayerBuffer> b);
    bool hasPlayerBuffer() const { return player != nullptr; }
    const std::shared_ptr<const PlayerBuffer>& playerBuffer() const { return player; }

    // ----- reglages (appliques au prochain bloc de calcul)
    void setMode(Mode m);
    void setSpeed(double ratio) { speed = ratio; }          // 1 = normal, negatif = a l'envers
    void setTranspose(double semitones);
    void setLock(bool on) { lock = on ? 1.0f : 0.0f; }
    void setLoop(bool on) { loop = on; }
    void setFreeze(bool on) { freeze = on; }
    void setMix(float wet) { mixTarget = wet; }           // mode effet : 0 = sec, 1 = traite
    void setGain(float g) { gainTarget = g; }

    // ----- transport du lecteur (comme les messages pv-play / pv-pause / pv-stop / pv-seek)
    void play();
    void pause();
    void stop();
    void seek(double fraction);
    bool isPlaying() const { return playing; }
    double position() const;       // 0..1 (mode effet gele : 0 = le plus ancien, 1 = instant du gel)

    /**
     * Scrub : tant que [active] est vrai, la position glisse vers [fraction] (0..1) et le son
     * reste audible, meme en pause ou a vitesse 0. Mode lecteur : position dans le fichier.
     * Mode effet avec gel : position dans l'historique enregistre avant le gel.
     */
    void setScrub(bool active, double fraction);
    bool isScrubbing() const { return scrubbing; }
    /** Temps de glissement du scrub et de la position pilotee, en millisecondes (0 = immediat). */
    void setScrubTime(double ms);
    /** Duree (s) de l'historique accessible en mode effet gele. */
    double historySeconds() const { return maxBackSamples() / sr; }
    bool takeEndFlag() { bool e = endFlag; endFlag = false; return e; }
    /** Vrai si le son est encore audible (lecture en cours ou fondu de sortie). */
    bool isSounding() const { return playing || gate > 0.0f || gateTarget > 0.0f; }

    Mode mode() const { return currentMode; }
    int latencySamples() const { return currentMode == Mode::Effect ? EFFECT_LATENCY : 0; }

    /** Traite n echantillons. Les entrees ne servent qu'en mode effet. */
    void process(const float* inL, const float* inR, float* outL, float* outR, int n);

    /** Remet a zero l'etat du vocodeur (phases, recouvrement). */
    void resetVocoder();

private:
    // un bloc d'analyse/synthese
    void computeFrame(double frontStart, double backStart);
    void readWindow(const float* buf, int64_t bufLen, double start, float tr, float* out) const;
    void readWindowCircular(const float* buf, double start, float tr, float* out) const;
    void playerAdvance();
    double maxBackSamples() const;
    void scrubStep();

    static void fft(float* re, float* im, bool inverse);
    static float rsqrt(float x);

    double sr = 48000.0;
    bool hasEffectBuffers = true;
    Mode currentMode = Mode::Player;

    // reglages
    double speed = 1.0, tr = 1.0;
    float lock = 1.0f;
    bool loop = false, freeze = false;

    // lecteur
    std::shared_ptr<const PlayerBuffer> player;
    double loc = PAD, start = PAD, end = PAD;
    bool playing = false, endFlag = false;
    int stopCountdown = -1;

    // mode effet : tampon circulaire en miroir (chaque echantillon ecrit deux fois)
    static constexpr int RING = 1 << 19;   // ~10 s a 48 kHz
    std::vector<float> ringL, ringR;
    int64_t written = 0;
    bool wasFrozen = false;
    int64_t freezeEnd = 0;          // nombre d'echantillons ecrits au moment du gel
    double frozenOffset = 0.0;      // recul dans l'historique (echantillons)

    // scrub
    bool scrubbing = false, gliding = false;
    double scrubFrac = 0.0;
    double scrubCoef = 0.12;
    double scrubMs = 80.0;
    double lastScrubSr = 0.0;
    std::vector<float> dryL, dryR;     // retard du signal sec = latence
    int dryPos = 0;

    // vocodeur
    std::vector<float> hann;
    std::vector<float> wFL, wFR, wB;
    std::vector<float> reL, imL, reR, imR, reB, imB;
    std::vector<float> prevRe, prevIm, uRe, uIm, qRe, qIm;
    std::vector<float> olaL, olaR;     // tampon de recouvrement-addition (2N, circulaire)
    int olaPos = 0;
    int hopCounter = 0;

    // rampes (equivalent des [line~] du patch)
    float gate = 0.0f, gateTarget = 0.0f, gateStep = 0.0f;
    float gain = 0.8f, gainTarget = 0.8f;
    float mix = 1.0f, mixTarget = 1.0f;
    float rampCoef = 0.0f;
    void setGate(float target);
};

} // namespace pvoc
