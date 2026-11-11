#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <memory>

#include "SpectralFx.h"

inline juce::String fxs(const char* utf8) { return juce::String::fromUTF8(utf8); }

class EffectProcessor : public juce::AudioProcessor,
                        public juce::ChangeBroadcaster,
                        private juce::Timer {
public:
    EffectProcessor();
    ~EffectProcessor() override;

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
    double getTailLengthSeconds() const override { return 0.1; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // ----- fichier modulateur de la cross-synthese
    void loadModulator(const juce::File& f);              // en arriere-plan
    juce::String loadModulatorNow(const juce::File& f);   // synchrone (tests), renvoie l'erreur
    void clearModulator();
    juce::File getModulatorFile() const;
    bool isLoading() const { return loading.load(); }
    juce::String getLastError() const;

    // ----- affichage
    const pvoc::SpectralFx& engine() const { return fx; }
    bool sidechainConnected() const { return sidechainOn.load(); }
    bool isFrozenNow() const { return frozenNow.load(); }
    int getActiveVoices() const { return fx.activeVoices(); }

    juce::AudioProcessorValueTreeState params;
    juce::MidiKeyboardState keyboardState;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void handleMidi(const juce::MidiMessage& m);
    void timerCallback() override;

    pvoc::SpectralFx fx;
    juce::AudioFormatManager formats;
    juce::ThreadPool loader { 1 };
    std::atomic<bool> loading { false };
    std::atomic<double> hostRate { 48000.0 };

    mutable juce::CriticalSection fileLock;
    juce::File modFile;
    juce::String lastError;
    std::shared_ptr<juce::AudioBuffer<float>> modOriginal;
    double modOriginalRate = 0.0;

    // passage du fichier au thread audio
    juce::SpinLock handoff;
    std::shared_ptr<const pvoc::PlayerBuffer> pendingMod, retiredMod;
    std::atomic<bool> hasPendingMod { false }, clearMod { false };
    void publishModulator(double rate);

    // clavier : 16 voix
    struct Slot { int note = -1; bool down = false, sustained = false; float vel = 1.0f; uint64_t age = 0; };
    std::array<Slot, pvoc::SpectralFx::MAX_VOICES> slots;
    uint64_t ageCounter = 0;
    bool pedal = false;
    float bendSemis = 0.0f;

    std::atomic<bool> sidechainOn { false }, frozenNow { false };
    std::vector<float> tmpL, tmpR;

    std::atomic<float>* p(const char* id) { return params.getRawParameterValue(id); }
    std::atomic<float> *pFreeze, *pPedal, *pPitch, *pShift, *pKeepMain, *pRobot, *pRobotHz, *pWhisper, *pBlur, *pGate,
        *pCross, *pCrossSmooth, *pCrossSource, *pMode, *pRef, *pBend, *pAttack, *pRelease, *pMix, *pGain;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EffectProcessor)
};
