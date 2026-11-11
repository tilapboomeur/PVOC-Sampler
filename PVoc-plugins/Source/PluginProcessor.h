#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <memory>

#include "PvocEngine.h"

/** Texte UTF-8 (accents) pour JUCE. */
inline juce::String fr(const char* utf8) { return juce::String::fromUTF8(utf8); }

class PvocProcessor : public juce::AudioProcessor,
                      public juce::ChangeBroadcaster,
                      private juce::AudioProcessorValueTreeState::Listener,
                      private juce::Timer {
public:
    PvocProcessor();
    ~PvocProcessor() override;

    // ----- AudioProcessor
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.2; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // ----- pour l'interface et les tests
    juce::AudioProcessorValueTreeState params;

    /** Charge un son en arriere-plan (appel depuis le thread de l'interface). */
    void loadFile(const juce::File& file);
    /** Variante synchrone (tests). Renvoie un message d'erreur, vide si tout va bien. */
    juce::String loadFileNow(const juce::File& file);

    void requestPlay()  { playReq = true; }
    void requestPause() { pauseReq = true; }
    void requestStop()  { stopReq = true; }
    void requestSeek(double fraction) { seekReq = (float) juce::jlimit(0.0, 1.0, fraction); }

    // scrub : le son suit le curseur pendant qu'on le deplace
    void beginScrub(double fraction) { scrubPos = (float) juce::jlimit(0.0, 1.0, fraction); scrubActive = true; }
    void moveScrub(double fraction)  { scrubPos = (float) juce::jlimit(0.0, 1.0, fraction); }
    void endScrub(double fraction)   { scrubPos = (float) juce::jlimit(0.0, 1.0, fraction); scrubActive = false; }
    bool scrubEnabled() const { return pScrub->load() > 0.5f; }
    /** Vrai si la position est pilotee par le parametre "Position" (automation / MIDI). */
    bool followEnabled() const { return pFollow->load() > 0.5f; }
    double getHistorySeconds() const { return historySec.load(); }

    double getPosition() const { return position.load(); }
    bool isPlayingNow() const { return playingNow.load(); }
    double getDurationSeconds() const { return durationSec.load(); }
    bool isLoading() const { return loading.load(); }
    juce::String getFileName() const;
    juce::String getFileInfo() const;
    juce::String getLastError() const;

private:
    void parameterChanged(const juce::String& id, float value) override;
    void timerCallback() override;
    void applyParameters();
    juce::String buildPlayerBuffer();   // depuis le son decode, a la frequence courante
    void updateLatency();

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    pvoc::Engine engine;
    juce::AudioFormatManager formats;
    juce::ThreadPool loader { 1 };

    // son decode (frequence d'origine), protege par dataLock
    juce::CriticalSection dataLock;
    std::shared_ptr<juce::AudioBuffer<float>> decoded;
    double decodedRate = 0.0;
    int decodedChannels = 0;
    juce::File currentFile;
    juce::String fileInfo, lastError;
    double builtForRate = 0.0;

    // passage du son au thread audio
    juce::SpinLock handoff;
    std::shared_ptr<const pvoc::PlayerBuffer> pending, retired;
    std::atomic<bool> hasPending { false };

    // commandes et etat partage avec l'interface
    std::atomic<bool> playReq { false }, pauseReq { false }, stopReq { false };
    std::atomic<float> seekReq { -1.0f };
    std::atomic<bool> scrubActive { false };
    std::atomic<float> scrubPos { 0.0f };
    std::atomic<double> historySec { 0.0 };
    std::atomic<double> position { 0.0 }, durationSec { 0.0 };
    std::atomic<bool> playingNow { false }, loading { false };
    std::atomic<int> modeNow { 0 };
    bool hostWasPlaying = false;
    double currentRate = 0.0;

    std::atomic<float>* pMode = nullptr;
    std::atomic<float>* pSpeed = nullptr;
    std::atomic<float>* pReverse = nullptr;
    std::atomic<float>* pTranspose = nullptr;
    std::atomic<float>* pLock = nullptr;
    std::atomic<float>* pLoop = nullptr;
    std::atomic<float>* pFreeze = nullptr;
    std::atomic<float>* pMix = nullptr;
    std::atomic<float>* pGain = nullptr;
    std::atomic<float>* pSync = nullptr;
    std::atomic<float>* pScrub = nullptr;
    std::atomic<float>* pPosition = nullptr;
    std::atomic<float>* pFollow = nullptr;
    std::atomic<float>* pSmooth = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PvocProcessor)
};
