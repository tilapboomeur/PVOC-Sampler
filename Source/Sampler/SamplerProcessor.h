#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <memory>

#include "../PvocEngine.h"
#include "Zones.h"

inline juce::String frs(const char* utf8) { return juce::String::fromUTF8(utf8); }

class SamplerProcessor : public juce::AudioProcessor,
                         public juce::ChangeBroadcaster,
                         private juce::Timer {
public:
    static constexpr int MAX_VOICES = 16;
    static constexpr int MAX_ZONES = 128;

    SamplerProcessor();
    ~SamplerProcessor() override;

    // ----- AudioProcessor
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 5.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // ----- zones (appels depuis le thread de l'interface)
    struct ZoneInfo {
        uint32_t id;
        juce::File file;
        pvoc::ZoneSettings settings;
        double durationSec;
        juce::String info;
    };
    std::vector<ZoneInfo> getZones() const;
    void addFiles(const juce::Array<juce::File>& files);
    /** Variante synchrone (tests). Renvoie l'id de la zone, 0 en cas d'erreur. */
    uint32_t addFileNow(const juce::File& file, int forcedRoot = -1);
    void updateZone(uint32_t id, const pvoc::ZoneSettings& s);
    void removeZone(uint32_t id);
    void removeAllZones();
    void autoSpread();
    bool isLoading() const { return loadingCount.load() > 0; }
    juce::String getLastError() const;

    int getActiveVoices() const { return activeVoices.load(); }
    /** Pour les tests : rend le LFO aleatoire reproductible. */
    void setRandomSeed(juce::int64 seed) { lfoRandom.setSeed(seed); }
    /** Notes en cours (pour l'affichage), bit n = note n. */
    std::array<uint64_t, 2> getSoundingNotes() const { return { soundingLo.load(), soundingHi.load() }; }

    juce::AudioProcessorValueTreeState params;
    juce::MidiKeyboardState keyboardState;   // clavier de l'interface

private:
    struct ZoneData {
        uint32_t id = 0;
        juce::File file;
        pvoc::ZoneSettings settings;
        std::shared_ptr<juce::AudioBuffer<float>> original;   // son decode, frequence d'origine
        double originalRate = 0.0;
        int channels = 0;
        std::shared_ptr<const pvoc::PlayerBuffer> buffer;    // a la frequence du moteur
        double builtRate = 0.0;
    };

    struct Voice {
        std::unique_ptr<pvoc::Engine> eng;
        bool active = false;
        int note = -1;
        uint32_t zoneId = 0;
        bool keyDown = false, sustained = false, releasing = false;
        float env = 0.0f, relStep = 0.0f;
        float velGain = 1.0f;
        uint64_t age = 0;
        pvoc::ZoneSettings s;
        std::shared_ptr<const pvoc::PlayerBuffer> buffer;
        // enveloppe de position : -1 = inactive, 0 A, 1 D, 2 S, 3 R
        int penvStage = -1;
        float penvT = 0.0f;           // ms ecoulees dans l'etape
        float relFrom = 0.0f;         // position au debut du relachement (0..1)
        float lastEnvPos = 0.0f;      // derniere position de l'enveloppe, sans LFO (0..1)
        bool penvDone = false;        // relachement de position termine
        double lfoPhase = 0.0;        // en cycles
        float lfoA = 0.0f, lfoB = 0.0f;
    };

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void timerCallback() override;
    void publishZones();                                  // reconstruit l'instantane pour le thread audio
    std::shared_ptr<const pvoc::PlayerBuffer> buildBuffer(const ZoneData& z, double rate, juce::String& info) const;
    void handleMidi(const juce::MidiMessage& m);
    void noteOn(int note, float velocity);
    void noteOff(int note);
    void releaseVoice(Voice& v);
    void startAmpRelease(Voice& v);
    float positionEnvelope(Voice& v, int n);   // position (0..1) pour ce bloc, LFO compris
    void killVoice(Voice& v);
    void renderVoices(float* outL, float* outR, int n);
    void applyVoiceSettings(Voice& v);

    // donnees cote interface
    mutable juce::CriticalSection zonesLock;
    std::vector<ZoneData> zones;
    uint32_t nextId = 1;
    juce::String lastError;
    juce::AudioFormatManager formats;
    juce::ThreadPool loader { 2 };
    std::atomic<int> loadingCount { 0 };
    std::atomic<double> hostRate { 0.0 };

    // passage des zones au thread audio
    juce::SpinLock handoff;
    std::shared_ptr<const pvoc::ZoneSet> pendingSet, currentSet;
    std::array<std::shared_ptr<const pvoc::ZoneSet>, 32> retiredSets;   // liberes par le timer
    std::atomic<bool> hasPending { false };
    std::vector<std::shared_ptr<const pvoc::PlayerBuffer>> keepAlive;   // liberation hors thread audio

    // voix (thread audio)
    std::array<Voice, MAX_VOICES> voices;
    uint64_t ageCounter = 0;
    bool sustainPedal = false;
    float pitchBend = 0.0f;   // en demi-tons
    double currentRate = 48000.0;
    std::vector<float> tmpL, tmpR;
    juce::Random lfoRandom;
    std::atomic<int> activeVoices { 0 };
    std::atomic<uint64_t> soundingLo { 0 }, soundingHi { 0 };

    std::atomic<float>* pVoices = nullptr;
    std::atomic<float>* pSpeed = nullptr;
    std::atomic<float>* pTranspose = nullptr;
    std::atomic<float>* pBendRange = nullptr;
    std::atomic<float>* pVelocity = nullptr;
    std::atomic<float>* pLock = nullptr;
    std::atomic<float>* pGain = nullptr;
    std::atomic<float>* pFollow = nullptr;
    std::atomic<float>* pPosition = nullptr;
    std::atomic<float>* pSmooth = nullptr;
    juce::SmoothedValue<float> masterGain;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SamplerProcessor)
};
