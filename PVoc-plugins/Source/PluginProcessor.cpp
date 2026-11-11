#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Resampler.h"

#include <thread>
#include <vector>

using APVTS = juce::AudioProcessorValueTreeState;

juce::AudioProcessorValueTreeState::ParameterLayout PvocProcessor::createLayout() {
    using namespace juce;
    APVTS::ParameterLayout l;
    l.add(std::make_unique<AudioParameterChoice>(ParameterID { "mode", 1 }, "Mode",
                                                 StringArray { "Lecteur", "Effet" }, 0));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID { "speed", 1 }, "Vitesse",
                                                NormalisableRange<float>(0.0f, 400.0f, 0.1f), 100.0f,
                                                AudioParameterFloatAttributes().withLabel("%")));
    l.add(std::make_unique<AudioParameterBool>(ParameterID { "reverse", 1 }, fr("À l'envers"), false));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID { "transpose", 1 }, "Hauteur",
                                                NormalisableRange<float>(-24.0f, 24.0f, 0.01f), 0.0f,
                                                AudioParameterFloatAttributes().withLabel("demi-tons")));
    l.add(std::make_unique<AudioParameterBool>(ParameterID { "lock", 1 }, "Verrouillage de phase", true));
    l.add(std::make_unique<AudioParameterBool>(ParameterID { "loop", 1 }, "Boucle", false));
    l.add(std::make_unique<AudioParameterBool>(ParameterID { "freeze", 1 }, "Gel", false));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID { "mix", 1 }, fr("Mélange"),
                                                NormalisableRange<float>(0.0f, 100.0f, 0.1f), 100.0f,
                                                AudioParameterFloatAttributes().withLabel("%")));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID { "gain", 1 }, "Volume",
                                                NormalisableRange<float>(-60.0f, 12.0f, 0.1f), 0.0f,
                                                AudioParameterFloatAttributes().withLabel("dB")));
    l.add(std::make_unique<AudioParameterBool>(ParameterID { "sync", 1 }, "Suivre le transport", false));
    l.add(std::make_unique<AudioParameterBool>(ParameterID { "scrub", 1 }, "Scrub", false));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID { "position", 1 }, "Position",
                                                NormalisableRange<float>(0.0f, 100.0f, 0.001f), 0.0f,
                                                AudioParameterFloatAttributes().withLabel("%")));
    l.add(std::make_unique<AudioParameterBool>(ParameterID { "follow", 1 }, fr("Position pilotée"), false));
    {
        NormalisableRange<float> r(0.0f, 1000.0f, 1.0f);
        r.setSkewForCentre(150.0f);   // plus de precision sur les petites valeurs
        l.add(std::make_unique<AudioParameterFloat>(ParameterID { "smooth", 1 }, "Lissage", r, 80.0f,
                                                    AudioParameterFloatAttributes().withLabel("ms")));
    }
    return l;
}

PvocProcessor::PvocProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Entrée", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Sortie", juce::AudioChannelSet::stereo(), true)),
      params(*this, nullptr, "LecteurPVoc", createLayout()) {
    formats.registerBasicFormats();
    pMode = params.getRawParameterValue("mode");
    pSpeed = params.getRawParameterValue("speed");
    pReverse = params.getRawParameterValue("reverse");
    pTranspose = params.getRawParameterValue("transpose");
    pLock = params.getRawParameterValue("lock");
    pLoop = params.getRawParameterValue("loop");
    pFreeze = params.getRawParameterValue("freeze");
    pMix = params.getRawParameterValue("mix");
    pGain = params.getRawParameterValue("gain");
    pSync = params.getRawParameterValue("sync");
    pScrub = params.getRawParameterValue("scrub");
    pPosition = params.getRawParameterValue("position");
    pFollow = params.getRawParameterValue("follow");
    pSmooth = params.getRawParameterValue("smooth");
    params.addParameterListener("mode", this);
    startTimerHz(10);
}

PvocProcessor::~PvocProcessor() {
    params.removeParameterListener("mode", this);
    loader.removeAllJobs(true, 10000);
}

// ------------------------------------------------------------------ audio

bool PvocProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto out = layouts.getMainOutputChannelSet();
    const auto in = layouts.getMainInputChannelSet();
    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono()) return false;
    return in.isDisabled() || in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono();
}

void PvocProcessor::prepareToPlay(double sampleRate, int) {
    engine.prepare(sampleRate);
    currentRate = sampleRate;
    applyParameters();
    updateLatency();
    // le son doit etre a la frequence de l'hote : reconstruction si elle a change
    bool rebuild = false;
    {
        const juce::ScopedLock sl(dataLock);
        rebuild = decoded != nullptr && std::abs(builtForRate - sampleRate) > 0.5;
    }
    if (rebuild) {
        loading = true;
        loader.addJob([this] {
            buildPlayerBuffer();
            loading = false;
            sendChangeMessage();
        });
    }
}

void PvocProcessor::applyParameters() {
    const int mode = (int) pMode->load();
    engine.setMode(mode == 1 ? pvoc::Engine::Mode::Effect : pvoc::Engine::Mode::Player);
    const double sp = pSpeed->load() / 100.0;
    engine.setSpeed(pReverse->load() > 0.5f ? -sp : sp);
    engine.setTranspose(pTranspose->load());
    engine.setLock(pLock->load() > 0.5f);
    engine.setLoop(pLoop->load() > 0.5f);
    engine.setFreeze(pFreeze->load() > 0.5f);
    engine.setMix(pMix->load() / 100.0f);
    const float db = pGain->load();
    engine.setGain(db <= -59.9f ? 0.0f : juce::Decibels::decibelsToGain(db));
    engine.setScrubTime(pSmooth->load());
}

void PvocProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    juce::ScopedNoDenormals noDenormals;

    // nouveau son pret ?
    if (hasPending.load(std::memory_order_acquire)) {
        const juce::SpinLock::ScopedTryLockType tl(handoff);
        // l'ancien son est garde dans "retired" et libere par le timer, hors du thread audio
        if (tl.isLocked() && pending != nullptr && retired == nullptr) {
            retired = engine.playerBuffer();
            engine.setPlayerBuffer(std::move(pending));
            pending = nullptr;
            hasPending = false;
        }
    }

    applyParameters();
    modeNow = (int) engine.mode();

    // transport de l'hote
    if (pSync->load() > 0.5f && engine.mode() == pvoc::Engine::Mode::Player) {
        bool hostPlaying = false;
        if (auto* ph = getPlayHead())
            if (auto pos = ph->getPosition()) hostPlaying = pos->getIsPlaying();
        if (hostPlaying && !hostWasPlaying) engine.play();
        if (!hostPlaying && hostWasPlaying) engine.pause();
        hostWasPlaying = hostPlaying;
    } else {
        hostWasPlaying = false;
    }

    // commandes de l'interface
    if (stopReq.exchange(false)) engine.stop();
    if (pauseReq.exchange(false)) engine.pause();
    if (playReq.exchange(false)) engine.play();
    const float s = seekReq.exchange(-1.0f);
    if (s >= 0.0f) engine.seek(s);
    const bool uiScrub = scrubActive.load() && pScrub->load() > 0.5f;
    if (pFollow->load() > 0.5f) {
        // Position pilotee par le parametre (automation, MIDI ou curseur) : meme glissement que le scrub.
        const double target = juce::jlimit(0.0, 1.0, pPosition->load() / 100.0);
        if (engine.mode() == pvoc::Engine::Mode::Effect) {
            engine.setScrub(pFreeze->load() > 0.5f, target);
        } else if (engine.isPlaying() || uiScrub) {
            if (!engine.isScrubbing()) engine.seek(target);   // depart exact a la position du parametre
            engine.setScrub(true, target);
        } else {
            engine.setScrub(false, target);
            engine.seek(target);          // a l'arret : la lecture repartira de la position du parametre
        }
    } else {
        engine.setScrub(uiScrub, scrubPos.load());
    }

    const int n = buffer.getNumSamples();
    const int inCh = getTotalNumInputChannels();
    const int outCh = buffer.getNumChannels();
    const float* inL = inCh > 0 ? buffer.getReadPointer(0) : nullptr;
    const float* inR = inCh > 1 ? buffer.getReadPointer(1) : inL;

    // traitement en place : on copie les entrees si besoin (le moteur lit avant d'ecrire, echantillon par echantillon)
    float* outL = buffer.getWritePointer(0);
    if (outCh > 1) {
        float* outR = buffer.getWritePointer(1);
        engine.process(inL, inR, outL, outR, n);
    } else {
        // sortie mono : moyenne des deux canaux, par tranches
        float tmpL[256], tmpR[256];
        for (int p = 0; p < n; p += 256) {
            const int k = std::min(256, n - p);
            engine.process(inL ? inL + p : nullptr, inR ? inR + p : nullptr, tmpL, tmpR, k);
            for (int i = 0; i < k; ++i) outL[p + i] = 0.5f * (tmpL[i] + tmpR[i]);
        }
    }
    for (int c = 2; c < outCh; ++c) buffer.clear(c, 0, n);

    position = engine.position();
    historySec = engine.historySeconds();
    playingNow = engine.isPlaying();
    if (engine.takeEndFlag()) playingNow = false;
}

void PvocProcessor::parameterChanged(const juce::String& id, float) {
    if (id == "mode") juce::MessageManager::callAsync([this] { updateLatency(); });
}

void PvocProcessor::updateLatency() {
    const bool effect = (int) pMode->load() == 1;
    const int lat = effect ? pvoc::Engine::EFFECT_LATENCY : 0;
    if (getLatencySamples() != lat) setLatencySamples(lat);
}

void PvocProcessor::timerCallback() {
    // libere l'ancien son hors du thread audio
    std::shared_ptr<const pvoc::PlayerBuffer> old;
    {
        const juce::SpinLock::ScopedLockType sl(handoff);
        old = std::move(retired);
        retired = nullptr;
    }
    old.reset();   // liberation de la memoire ici, en dehors du verrou
}

// ------------------------------------------------------------------ fichiers

void PvocProcessor::loadFile(const juce::File& file) {
    loading = true;
    sendChangeMessage();
    loader.addJob([this, file] {
        const auto err = loadFileNow(file);
        juce::ignoreUnused(err);
        loading = false;
        sendChangeMessage();
    });
}

juce::String PvocProcessor::loadFileNow(const juce::File& file) {
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr) {
        const juce::ScopedLock sl(dataLock);
        lastError = fr("Format non reconnu : ") + file.getFileName();
        return lastError;
    }
    if (reader->lengthInSamples <= 0 || reader->lengthInSamples > (juce::int64) 600000000) {
        const juce::ScopedLock sl(dataLock);
        lastError = fr("Fichier vide ou trop long : ") + file.getFileName();
        return lastError;
    }
    const int chans = (int) juce::jlimit(1u, 2u, reader->numChannels);
    auto buf = std::make_shared<juce::AudioBuffer<float>>(2, (int) reader->lengthInSamples);
    reader->read(buf.get(), 0, (int) reader->lengthInSamples, 0, true, chans > 1);
    if (chans == 1) buf->copyFrom(1, 0, *buf, 0, 0, buf->getNumSamples());

    {
        const juce::ScopedLock sl(dataLock);
        decoded = buf;
        decodedRate = reader->sampleRate;
        decodedChannels = (int) reader->numChannels;
        currentFile = file;
        lastError.clear();
    }
    return buildPlayerBuffer();
}

juce::String PvocProcessor::buildPlayerBuffer() {
    std::shared_ptr<juce::AudioBuffer<float>> src;
    double srcRate, dstRate = currentRate > 0 ? currentRate : 48000.0;
    int channels;
    {
        const juce::ScopedLock sl(dataLock);
        src = decoded;
        srcRate = decodedRate;
        channels = decodedChannels;
    }
    if (src == nullptr) return {};

    const int pad = pvoc::Engine::PAD;
    const int64_t inLen = src->getNumSamples();
    const int isrc = (int) std::lround(srcRate), idst = (int) std::lround(dstRate);
    const bool resample = isrc != idst;
    pvoc::Resampler rs(std::max(1, isrc), std::max(1, idst));
    const int64_t outLen = resample ? rs.outputLength(inLen) : inLen;

    auto pb = std::make_shared<pvoc::PlayerBuffer>();
    pb->length = outLen;
    for (int c = 0; c < 2; ++c) {
        auto& dst = c == 0 ? pb->left : pb->right;
        dst.assign((size_t) (outLen + 2 * pad + 8), 0.0f);
        const float* in = src->getReadPointer(c);
        if (!resample) {
            std::copy(in, in + inLen, dst.begin() + pad);
        } else {
            // calcul reparti sur plusieurs coeurs
            const int nt = (int) juce::jlimit(1u, 8u, std::thread::hardware_concurrency());
            std::vector<std::thread> th;
            const int64_t chunk = (outLen + nt - 1) / nt;
            float* out = dst.data() + pad;
            for (int t = 0; t < nt; ++t) {
                const int64_t a = t * chunk, b = std::min(outLen, a + chunk);
                if (a < b) th.emplace_back([&rs, in, inLen, out, a, b] { rs.process(in, inLen, out, a, b); });
            }
            for (auto& x : th) x.join();
        }
    }

    {
        const juce::ScopedLock sl(dataLock);
        builtForRate = dstRate;
        fileInfo = juce::String(channels == 1 ? fr("mono") : fr("stéréo")) + " · "
                   + juce::String(srcRate / 1000.0, 1) + " kHz";
        if (resample)
            fileInfo += fr(" → rééchantillonné à ") + juce::String(dstRate / 1000.0, 1) + " kHz";
    }
    durationSec = (double) outLen / dstRate;
    {
        const juce::SpinLock::ScopedLockType sl(handoff);
        pending = pb;
        hasPending.store(true, std::memory_order_release);
    }
    return {};
}

juce::String PvocProcessor::getFileName() const {
    const juce::ScopedLock sl(dataLock);
    return currentFile == juce::File() ? juce::String() : currentFile.getFileName();
}

juce::String PvocProcessor::getFileInfo() const {
    const juce::ScopedLock sl(dataLock);
    return fileInfo;
}

juce::String PvocProcessor::getLastError() const {
    const juce::ScopedLock sl(dataLock);
    return lastError;
}

// ------------------------------------------------------------------ etat (sauvegarde dans le projet)

void PvocProcessor::getStateInformation(juce::MemoryBlock& destData) {
    auto state = params.copyState();
    {
        const juce::ScopedLock sl(dataLock);
        state.setProperty("file", currentFile.getFullPathName(), nullptr);
    }
    if (auto xml = state.createXml()) copyXmlToBinary(*xml, destData);
}

void PvocProcessor::setStateInformation(const void* data, int sizeInBytes) {
    auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr || !xml->hasTagName(params.state.getType())) return;
    auto state = juce::ValueTree::fromXml(*xml);
    const juce::String path = state.getProperty("file", juce::String());
    params.replaceState(state);
    bool alreadyLoaded = false;
    {
        const juce::ScopedLock sl(dataLock);
        alreadyLoaded = decoded != nullptr && currentFile.getFullPathName() == path;
    }
    if (path.isNotEmpty() && !alreadyLoaded) {
        const juce::File f(path);
        if (f.existsAsFile()) loadFile(f);
        else {
            const juce::ScopedLock sl(dataLock);
            lastError = fr("Fichier introuvable : ") + path;
        }
    }
    juce::MessageManager::callAsync([this] { updateLatency(); });
}

juce::AudioProcessorEditor* PvocProcessor::createEditor() { return new PvocEditor(*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PvocProcessor(); }
