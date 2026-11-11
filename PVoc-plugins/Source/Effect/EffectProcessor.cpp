#include "EffectProcessor.h"
#include "EffectEditor.h"
#include "../Resampler.h"

#include <cmath>
#include <thread>

using APVTS = juce::AudioProcessorValueTreeState;

APVTS::ParameterLayout EffectProcessor::createLayout() {
    using namespace juce;
    APVTS::ParameterLayout l;
    auto pct = [&](const char* id, const char* name, float def) {
        l.add(std::make_unique<AudioParameterFloat>(ParameterID { id, 1 }, fxs(name), NormalisableRange<float>(0.0f, 100.0f, 0.1f), def,
                                                    AudioParameterFloatAttributes().withLabel("%")));
    };
    l.add(std::make_unique<AudioParameterBool>(ParameterID { "freeze", 1 }, "Gel", false));
    l.add(std::make_unique<AudioParameterBool>(ParameterID { "pedalFreeze", 1 }, fxs("Pédale = gel"), true));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID { "pitch", 1 }, "Hauteur", NormalisableRange<float>(-24.0f, 24.0f, 0.01f), 0.0f,
                                                AudioParameterFloatAttributes().withLabel("demi-tons")));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID { "fshift", 1 }, fxs("Décalage de fréquence"),
                                                NormalisableRange<float>(-2000.0f, 2000.0f, 0.1f, 0.4f, true), 0.0f,
                                                AudioParameterFloatAttributes().withLabel("Hz")));
    l.add(std::make_unique<AudioParameterBool>(ParameterID { "keepMain", 1 }, "Garder la voix principale", true));
    pct("robot", "Robotisation", 0.0f);
    {
        NormalisableRange<float> r(30.0f, 1000.0f, 0.1f);
        r.setSkewForCentre(150.0f);
        l.add(std::make_unique<AudioParameterFloat>(ParameterID { "robotHz", 1 }, "Hauteur du robot", r, 110.0f,
                                                    AudioParameterFloatAttributes().withLabel("Hz")));
    }
    pct("whisper", "Chuchotement", 0.0f);
    pct("blur", "Flou spectral", 0.0f);
    l.add(std::make_unique<AudioParameterFloat>(ParameterID { "gate", 1 }, "Porte spectrale", NormalisableRange<float>(-90.0f, 0.0f, 0.1f), -90.0f,
                                                AudioParameterFloatAttributes().withLabel("dB")));
    pct("cross", "Cross-synthèse", 0.0f);
    pct("crossSmooth", "Lissage de l'enveloppe", 40.0f);
    l.add(std::make_unique<AudioParameterChoice>(ParameterID { "crossSource", 1 }, "Source du modulateur",
                                                 StringArray { "Sidechain", "Fichier" }, 0));
    l.add(std::make_unique<AudioParameterChoice>(ParameterID { "kbMode", 1 }, "Clavier",
                                                 StringArray { "Harmoniseur", fxs("Robot (vocodeur)"), "Les deux" }, 0));
    l.add(std::make_unique<AudioParameterInt>(ParameterID { "refNote", 1 }, fxs("Note de référence"), 0, 127, 60));
    l.add(std::make_unique<AudioParameterInt>(ParameterID { "bend", 1 }, "Plage du pitch bend", 0, 24, 2,
                                              AudioParameterIntAttributes().withLabel("demi-tons")));
    {
        NormalisableRange<float> a(1.0f, 2000.0f, 0.1f);
        a.setSkewForCentre(100.0f);
        l.add(std::make_unique<AudioParameterFloat>(ParameterID { "attack", 1 }, "Attaque", a, 10.0f,
                                                    AudioParameterFloatAttributes().withLabel("ms")));
        NormalisableRange<float> r(1.0f, 5000.0f, 0.1f);
        r.setSkewForCentre(400.0f);
        l.add(std::make_unique<AudioParameterFloat>(ParameterID { "release", 1 }, fxs("Relâchement"), r, 200.0f,
                                                    AudioParameterFloatAttributes().withLabel("ms")));
    }
    pct("mix", "Mélange", 100.0f);
    l.add(std::make_unique<AudioParameterFloat>(ParameterID { "gain", 1 }, "Volume", NormalisableRange<float>(-60.0f, 12.0f, 0.1f), 0.0f,
                                                AudioParameterFloatAttributes().withLabel("dB")));
    return l;
}

EffectProcessor::EffectProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput(fxs("Entrée"), juce::AudioChannelSet::stereo(), true)
                         .withOutput("Sortie", juce::AudioChannelSet::stereo(), true)
                         .withInput("Sidechain", juce::AudioChannelSet::stereo(), false)),
      params(*this, nullptr, "EffetPVoc", createLayout()) {
    formats.registerBasicFormats();
    pFreeze = p("freeze"); pPedal = p("pedalFreeze"); pPitch = p("pitch"); pShift = p("fshift"); pKeepMain = p("keepMain");
    pRobot = p("robot"); pRobotHz = p("robotHz"); pWhisper = p("whisper"); pBlur = p("blur"); pGate = p("gate");
    pCross = p("cross"); pCrossSmooth = p("crossSmooth"); pCrossSource = p("crossSource"); pMode = p("kbMode");
    pRef = p("refNote"); pBend = p("bend"); pAttack = p("attack"); pRelease = p("release"); pMix = p("mix"); pGain = p("gain");
    setLatencySamples(pvoc::SpectralFx::LATENCY);
    startTimerHz(10);
}

EffectProcessor::~EffectProcessor() {
    stopTimer();
    loader.removeAllJobs(true, 20000);
}

bool EffectProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono()) return false;
    if (in != out && !(in == juce::AudioChannelSet::mono() && out == juce::AudioChannelSet::stereo())) return false;
    if (layouts.inputBuses.size() > 1) {
        const auto sc = layouts.getChannelSet(true, 1);
        if (!sc.isDisabled() && sc != juce::AudioChannelSet::mono() && sc != juce::AudioChannelSet::stereo()) return false;
    }
    return true;
}

void EffectProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    fx.prepare(sampleRate);
    hostRate = sampleRate;
    tmpL.assign((size_t) juce::jmax(512, samplesPerBlock), 0.0f);
    tmpR.assign(tmpL.size(), 0.0f);
    setLatencySamples(pvoc::SpectralFx::LATENCY);
    keyboardState.reset();
    for (auto& s : slots) s = {};
    pedal = false;
    // le fichier modulateur doit etre a la frequence de l'hote
    bool rebuild;
    {
        const juce::ScopedLock sl(fileLock);
        rebuild = modOriginal != nullptr;
    }
    if (rebuild) publishModulator(sampleRate);
}

// ------------------------------------------------------------------ MIDI

void EffectProcessor::handleMidi(const juce::MidiMessage& m) {
    if (m.isNoteOn()) {
        Slot* target = nullptr;
        for (auto& s : slots) if (s.note == m.getNoteNumber() && (s.down || s.sustained)) target = &s;   // meme note : on la reprend
        if (target == nullptr) for (auto& s : slots) if (!s.down && !s.sustained) { target = &s; break; }
        if (target == nullptr) {   // toutes les voix prises : on remplace la plus ancienne
            target = &slots[0];
            for (auto& s : slots) if (s.age < target->age) target = &s;
        }
        target->note = m.getNoteNumber();
        target->down = true;
        target->sustained = false;
        target->vel = m.getFloatVelocity();
        target->age = ++ageCounter;
    } else if (m.isNoteOff()) {
        for (auto& s : slots)
            if (s.note == m.getNoteNumber() && s.down) {
                s.down = false;
                s.sustained = pedal && pPedal->load() < 0.5f;   // pedale en mode sustain : la note tient
            }
    } else if (m.isSustainPedalOn()) {
        pedal = true;
    } else if (m.isSustainPedalOff()) {
        pedal = false;
        for (auto& s : slots) s.sustained = false;
    } else if (m.isPitchWheel()) {
        bendSemis = (float) (m.getPitchWheelValue() - 8192) / 8192.0f * pBend->load();
    } else if (m.isAllNotesOff() || m.isAllSoundOff()) {
        for (auto& s : slots) s = {};
        pedal = false;
    }
}

// ------------------------------------------------------------------ audio

void EffectProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();

    // nouveau fichier modulateur ?
    if (hasPendingMod.load(std::memory_order_acquire) || clearMod.load()) {
        const juce::SpinLock::ScopedTryLockType tl(handoff);
        if (tl.isLocked() && retiredMod == nullptr) {
            retiredMod = fx.modulatorFile();   // libere par le timer
            if (clearMod.exchange(false)) fx.setModulatorFile(nullptr);
            if (hasPendingMod.exchange(false)) fx.setModulatorFile(std::move(pendingMod));
            pendingMod = nullptr;
        }
    }

    keyboardState.processNextMidiBuffer(midi, 0, n, true);
    for (const auto meta : midi) handleMidi(meta.getMessage());

    // reglages
    pvoc::SpectralFx::Params pr;
    pr.freeze = pFreeze->load() > 0.5f || (pedal && pPedal->load() > 0.5f);
    pr.pitchSemis = pPitch->load();
    pr.fshiftHz = pShift->load();
    pr.keepMain = pKeepMain->load() > 0.5f;
    pr.robot = pRobot->load() / 100.0f;
    pr.robotHz = pRobotHz->load();
    pr.whisper = pWhisper->load() / 100.0f;
    pr.blur = pBlur->load() / 100.0f;
    pr.gateDb = pGate->load();
    pr.cross = pCross->load() / 100.0f;
    pr.crossSmooth = pCrossSmooth->load() / 100.0f;
    pr.crossSource = (int) pCrossSource->load();
    pr.attackMs = pAttack->load();
    pr.releaseMs = pRelease->load();
    pr.mix = pMix->load() / 100.0f;
    pr.gain = pGain->load() <= -59.9f ? 0.0f : juce::Decibels::decibelsToGain(pGain->load());
    fx.setParams(pr);
    frozenNow = pr.freeze;

    // voix du clavier
    pvoc::SpectralFx::VoiceIn v[pvoc::SpectralFx::MAX_VOICES];
    const int ref = (int) pRef->load();
    for (size_t i = 0; i < slots.size(); ++i) {
        const auto& s = slots[i];
        v[i].gate = s.note >= 0 && (s.down || s.sustained);
        v[i].semis = (float) (s.note - ref) + bendSemis;
        v[i].robotHz = (float) (440.0 * std::pow(2.0, (s.note - 69 + bendSemis) / 12.0));
        v[i].level = 0.25f + 0.75f * s.vel;
    }
    const int mode = (int) pMode->load();
    fx.setVoices(v, (int) slots.size(), mode != 1, mode != 0);

    // entrees : principale et sidechain
    auto mainIn = getBusBuffer(buffer, true, 0);
    const int inCh = mainIn.getNumChannels();
    const float* inL = inCh > 0 ? mainIn.getReadPointer(0) : nullptr;
    const float* inR = inCh > 1 ? mainIn.getReadPointer(1) : inL;
    const float* scL = nullptr;
    const float* scR = nullptr;
    if (getBusCount(true) > 1) {
        if (auto* bus = getBus(true, 1); bus != nullptr && bus->isEnabled()) {
            auto sc = getBusBuffer(buffer, true, 1);
            if (sc.getNumChannels() > 0) {
                scL = sc.getReadPointer(0);
                scR = sc.getNumChannels() > 1 ? sc.getReadPointer(1) : scL;
            }
        }
    }
    sidechainOn = scL != nullptr;

    auto mainOut = getBusBuffer(buffer, false, 0);
    float* outL = mainOut.getWritePointer(0);
    if (mainOut.getNumChannels() > 1) {
        // le moteur lit chaque echantillon avant de l'ecrire : le traitement en place est sur
        fx.process(inL, inR, scL, scR, outL, mainOut.getWritePointer(1), n);
    } else {
        for (int pos = 0; pos < n;) {
            const int k = juce::jmin(n - pos, (int) tmpL.size());
            fx.process(inL ? inL + pos : nullptr, inR ? inR + pos : nullptr, scL ? scL + pos : nullptr, scR ? scR + pos : nullptr,
                       tmpL.data(), tmpR.data(), k);
            for (int i = 0; i < k; ++i) outL[pos + i] = 0.5f * (tmpL[(size_t) i] + tmpR[(size_t) i]);
            pos += k;
        }
    }
    for (int c = mainOut.getNumChannels(); c < buffer.getNumChannels(); ++c) buffer.clear(c, 0, n);
}

// ------------------------------------------------------------------ fichier modulateur

void EffectProcessor::publishModulator(double rate) {
    std::shared_ptr<juce::AudioBuffer<float>> src;
    double srcRate;
    {
        const juce::ScopedLock sl(fileLock);
        src = modOriginal;
        srcRate = modOriginalRate;
    }
    if (src == nullptr) return;
    const int pad = pvoc::Engine::PAD;
    const int64_t inLen = src->getNumSamples();
    const int is = (int) std::lround(srcRate), id = (int) std::lround(rate);
    pvoc::Resampler rs(juce::jmax(1, is), juce::jmax(1, id));
    const int64_t outLen = is != id ? rs.outputLength(inLen) : inLen;
    auto pb = std::make_shared<pvoc::PlayerBuffer>();
    pb->length = outLen;
    for (int c = 0; c < 2; ++c) {
        auto& dst = c == 0 ? pb->left : pb->right;
        dst.assign((size_t) (outLen + 2 * pad + 8), 0.0f);
        const float* in = src->getReadPointer(c);
        if (is == id) std::copy(in, in + inLen, dst.begin() + pad);
        else rs.process(in, inLen, dst.data() + pad, 0, outLen);
    }
    const juce::SpinLock::ScopedLockType sl(handoff);
    pendingMod = pb;
    hasPendingMod = true;
}

juce::String EffectProcessor::loadModulatorNow(const juce::File& f) {
    std::unique_ptr<juce::AudioFormatReader> r(formats.createReaderFor(f));
    if (r == nullptr || r->lengthInSamples <= 0 || r->lengthInSamples > (juce::int64) 100000000) {
        const juce::ScopedLock sl(fileLock);
        lastError = fxs("Impossible de lire : ") + f.getFileName();
        return lastError;
    }
    auto buf = std::make_shared<juce::AudioBuffer<float>>(2, (int) r->lengthInSamples);
    const int ch = (int) juce::jlimit(1u, 2u, r->numChannels);
    r->read(buf.get(), 0, (int) r->lengthInSamples, 0, true, ch > 1);
    if (ch == 1) buf->copyFrom(1, 0, *buf, 0, 0, buf->getNumSamples());
    {
        const juce::ScopedLock sl(fileLock);
        modOriginal = buf;
        modOriginalRate = r->sampleRate;
        modFile = f;
        lastError.clear();
    }
    publishModulator(hostRate.load());
    return {};
}

void EffectProcessor::loadModulator(const juce::File& f) {
    loading = true;
    sendChangeMessage();
    loader.addJob([this, f] {
        loadModulatorNow(f);
        loading = false;
        sendChangeMessage();
    });
}

void EffectProcessor::clearModulator() {
    {
        const juce::ScopedLock sl(fileLock);
        modOriginal = nullptr;
        modFile = juce::File();
    }
    clearMod = true;
    sendChangeMessage();
}

juce::File EffectProcessor::getModulatorFile() const {
    const juce::ScopedLock sl(fileLock);
    return modFile;
}

juce::String EffectProcessor::getLastError() const {
    const juce::ScopedLock sl(fileLock);
    return lastError;
}

void EffectProcessor::timerCallback() {
    std::shared_ptr<const pvoc::PlayerBuffer> old;
    {
        const juce::SpinLock::ScopedLockType sl(handoff);
        old = std::move(retiredMod);
        retiredMod = nullptr;
    }
    old.reset();   // liberation hors du thread audio
}

// ------------------------------------------------------------------ etat

void EffectProcessor::getStateInformation(juce::MemoryBlock& destData) {
    auto state = params.copyState();
    state.setProperty("modFile", getModulatorFile().getFullPathName(), nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary(*xml, destData);
}

void EffectProcessor::setStateInformation(const void* data, int sizeInBytes) {
    auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr || !xml->hasTagName(params.state.getType())) return;
    auto state = juce::ValueTree::fromXml(*xml);
    const juce::String path = state.getProperty("modFile", juce::String());
    params.replaceState(state);
    if (path.isNotEmpty() && juce::File(path) != getModulatorFile()) {
        const juce::File f(path);
        if (f.existsAsFile()) loadModulator(f);
        else {
            const juce::ScopedLock sl(fileLock);
            lastError = fxs("Son modulateur introuvable : ") + f.getFileName();
        }
    }
}

juce::AudioProcessorEditor* EffectProcessor::createEditor() { return new EffectEditor(*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new EffectProcessor(); }
