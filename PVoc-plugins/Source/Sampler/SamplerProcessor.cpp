#include "SamplerProcessor.h"
#include "SamplerEditor.h"
#include "../Resampler.h"

#include <cmath>
#include <map>
#include <thread>

using APVTS = juce::AudioProcessorValueTreeState;

APVTS::ParameterLayout SamplerProcessor::createLayout() {
    using namespace juce;
    APVTS::ParameterLayout l;
    l.add(std::make_unique<AudioParameterInt>(ParameterID { "voices", 1 }, "Voix", 1, MAX_VOICES, 8));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID { "speed", 1 }, "Vitesse",
                                                NormalisableRange<float>(0.0f, 400.0f, 0.1f), 100.0f,
                                                AudioParameterFloatAttributes().withLabel("%")));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID { "transpose", 1 }, "Hauteur",
                                                NormalisableRange<float>(-24.0f, 24.0f, 0.01f), 0.0f,
                                                AudioParameterFloatAttributes().withLabel("demi-tons")));
    l.add(std::make_unique<AudioParameterInt>(ParameterID { "bend", 1 }, "Pitch bend", 0, 24, 2,
                                              AudioParameterIntAttributes().withLabel("demi-tons")));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID { "velocity", 1 }, frs("Sensibilité vélocité"),
                                                NormalisableRange<float>(0.0f, 100.0f, 1.0f), 70.0f,
                                                AudioParameterFloatAttributes().withLabel("%")));
    l.add(std::make_unique<AudioParameterBool>(ParameterID { "lock", 1 }, "Verrouillage de phase", true));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID { "gain", 1 }, "Volume",
                                                NormalisableRange<float>(-60.0f, 12.0f, 0.1f), -6.0f,
                                                AudioParameterFloatAttributes().withLabel("dB")));
    l.add(std::make_unique<AudioParameterBool>(ParameterID { "follow", 1 }, frs("Position pilotée"), false));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID { "position", 1 }, "Position",
                                                NormalisableRange<float>(0.0f, 100.0f, 0.001f), 0.0f,
                                                AudioParameterFloatAttributes().withLabel("%")));
    {
        NormalisableRange<float> r(0.0f, 1000.0f, 1.0f);
        r.setSkewForCentre(150.0f);
        l.add(std::make_unique<AudioParameterFloat>(ParameterID { "smooth", 1 }, "Lissage", r, 80.0f,
                                                    AudioParameterFloatAttributes().withLabel("ms")));
    }
    // enveloppe de position : reglages globaux automatisables, appliques par-dessus ceux de chaque son
    const char* ofsId[] = { "envStart", "envPeak", "envSustain", "envEnd" };
    const char* ofsName[] = { "Env. départ", "Env. pic", "Env. maintien", "Env. fin" };
    for (int i = 0; i < 4; ++i)
        l.add(std::make_unique<AudioParameterFloat>(ParameterID { ofsId[i], 1 }, frs(ofsName[i]),
                                                    NormalisableRange<float>(-100.0f, 100.0f, 0.1f), 0.0f,
                                                    AudioParameterFloatAttributes().withLabel("%")));
    const char* timeId[] = { "envA", "envD", "envR" };
    const char* timeName[] = { "Env. durée A", "Env. durée D", "Env. durée R" };
    for (int i = 0; i < 3; ++i) {
        NormalisableRange<float> r(10.0f, 1000.0f, 0.1f);
        r.setSkewForCentre(100.0f);
        l.add(std::make_unique<AudioParameterFloat>(ParameterID { timeId[i], 1 }, frs(timeName[i]), r, 100.0f,
                                                    AudioParameterFloatAttributes().withLabel("%")));
    }
    {
        NormalisableRange<float> r(10.0f, 1000.0f, 0.1f);
        r.setSkewForCentre(100.0f);
        l.add(std::make_unique<AudioParameterFloat>(ParameterID { "lfoRate", 1 }, frs("LFO vitesse"), r, 100.0f,
                                                    AudioParameterFloatAttributes().withLabel("%")));
    }
    l.add(std::make_unique<AudioParameterFloat>(ParameterID { "lfoDepth", 1 }, frs("LFO profondeur"),
                                                NormalisableRange<float>(0.0f, 400.0f, 0.1f), 100.0f,
                                                AudioParameterFloatAttributes().withLabel("%")));
    return l;
}

SamplerProcessor::SamplerProcessor()
    : AudioProcessor(BusesProperties().withOutput("Sortie", juce::AudioChannelSet::stereo(), true)),
      params(*this, nullptr, "SamplerPVoc", createLayout()) {
    formats.registerBasicFormats();
    pVoices = params.getRawParameterValue("voices");
    pSpeed = params.getRawParameterValue("speed");
    pTranspose = params.getRawParameterValue("transpose");
    pBendRange = params.getRawParameterValue("bend");
    pVelocity = params.getRawParameterValue("velocity");
    pLock = params.getRawParameterValue("lock");
    pGain = params.getRawParameterValue("gain");
    pFollow = params.getRawParameterValue("follow");
    pPosition = params.getRawParameterValue("position");
    pSmooth = params.getRawParameterValue("smooth");
    pEnvOfs[0] = params.getRawParameterValue("envStart");
    pEnvOfs[1] = params.getRawParameterValue("envPeak");
    pEnvOfs[2] = params.getRawParameterValue("envSustain");
    pEnvOfs[3] = params.getRawParameterValue("envEnd");
    pEnvTime[0] = params.getRawParameterValue("envA");
    pEnvTime[1] = params.getRawParameterValue("envD");
    pEnvTime[2] = params.getRawParameterValue("envR");
    pLfoRate = params.getRawParameterValue("lfoRate");
    pLfoDepth = params.getRawParameterValue("lfoDepth");
    for (auto& v : voices) v.eng = std::make_unique<pvoc::Engine>(false);
    currentSet = std::make_shared<const pvoc::ZoneSet>();
    startTimerHz(10);
}

SamplerProcessor::~SamplerProcessor() {
    stopTimer();
    loader.removeAllJobs(true, 20000);
}

bool SamplerProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void SamplerProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    currentRate = sampleRate > 0 ? sampleRate : 48000.0;
    hostRate = currentRate;
    for (auto& v : voices) {
        v.eng->prepare(currentRate);
        v.active = false;
        v.buffer.reset();
    }
    tmpL.assign((size_t) juce::jmax(512, samplesPerBlock), 0.0f);
    tmpR.assign(tmpL.size(), 0.0f);
    masterGain.reset(currentRate, 0.02);
    for (int i = 0; i < 4; ++i) envOfsCur[i] = pEnvOfs[i]->load();
    masterGain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(pGain->load(), -60.0f));
    keyboardState.reset();

    // les sons doivent etre a la frequence de l'hote : reconstruction en arriere-plan si besoin
    bool rebuild = false;
    {
        const juce::ScopedLock sl(zonesLock);
        for (auto& z : zones) rebuild |= z.original != nullptr && std::abs(z.builtRate - currentRate) > 0.5;
    }
    if (rebuild) {
        ++loadingCount;
        loader.addJob([this] {
            std::vector<ZoneData> copy;
            {
                const juce::ScopedLock sl(zonesLock);
                copy = zones;
            }
            const double rate = hostRate.load();
            for (auto& z : copy) {
                juce::String info;
                auto b = buildBuffer(z, rate, info);
                const juce::ScopedLock sl(zonesLock);
                for (auto& zz : zones)
                    if (zz.id == z.id) { zz.buffer = b; zz.builtRate = rate; }
            }
            publishZones();
            --loadingCount;
            sendChangeMessage();
        });
    }
}

// ------------------------------------------------------------------ chargement des sons

std::shared_ptr<const pvoc::PlayerBuffer> SamplerProcessor::buildBuffer(const ZoneData& z, double rate,
                                                                        juce::String& info) const {
    const int pad = pvoc::Engine::PAD;
    const auto& src = *z.original;
    const int64_t inLen = src.getNumSamples();
    const int isrc = (int) std::lround(z.originalRate), idst = (int) std::lround(rate);
    const bool resample = isrc != idst;
    pvoc::Resampler rs(juce::jmax(1, isrc), juce::jmax(1, idst));
    const int64_t outLen = resample ? rs.outputLength(inLen) : inLen;
    auto pb = std::make_shared<pvoc::PlayerBuffer>();
    pb->length = outLen;
    for (int c = 0; c < 2; ++c) {
        auto& dst = c == 0 ? pb->left : pb->right;
        dst.assign((size_t) (outLen + 2 * pad + 8), 0.0f);
        const float* in = src.getReadPointer(c);
        if (!resample) {
            std::copy(in, in + inLen, dst.begin() + pad);
        } else {
            const int nt = (int) juce::jlimit(1u, 4u, std::thread::hardware_concurrency());
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
    info = juce::String(z.channels == 1 ? frs("mono") : frs("stéréo")) + " · " + juce::String(z.originalRate / 1000.0, 1)
           + " kHz";
    if (resample) info += frs(" → ") + juce::String(rate / 1000.0, 1) + " kHz";
    return pb;
}

uint32_t SamplerProcessor::addFileNow(const juce::File& file, int forcedRoot) {
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->lengthInSamples > (juce::int64) 300000000) {
        const juce::ScopedLock sl(zonesLock);
        lastError = frs("Impossible de lire : ") + file.getFileName();
        return 0;
    }
    ZoneData z;
    z.file = file;
    z.channels = (int) reader->numChannels;
    z.originalRate = reader->sampleRate;
    const int chans = (int) juce::jlimit(1u, 2u, reader->numChannels);
    z.original = std::make_shared<juce::AudioBuffer<float>>(2, (int) reader->lengthInSamples);
    reader->read(z.original.get(), 0, (int) reader->lengthInSamples, 0, true, chans > 1);
    if (chans == 1) z.original->copyFrom(1, 0, *z.original, 0, 0, z.original->getNumSamples());

    const int guessed = pvoc::noteFromFileName(file.getFileName());
    z.settings.root = forcedRoot >= 0 ? forcedRoot : (guessed >= 0 ? guessed : 60);
    const double rate = hostRate.load() > 0 ? hostRate.load() : 48000.0;
    juce::String info;
    z.buffer = buildBuffer(z, rate, info);
    z.builtRate = rate;

    uint32_t id;
    {
        const juce::ScopedLock sl(zonesLock);
        if ((int) zones.size() >= MAX_ZONES) {
            lastError = frs("Nombre maximal de sons atteint (128).");
            return 0;
        }
        id = nextId++;
        z.id = id;
        zones.push_back(std::move(z));
        std::vector<ZoneData*> ptrs;
        for (auto& zz : zones) ptrs.push_back(&zz);
        pvoc::spreadRanges(ptrs, false);
        if (autoSort.load() && !restoring) sortLocked(SortMode::ByNote);
        lastError.clear();
    }
    publishZones();
    sendChangeMessage();
    return id;
}

void SamplerProcessor::addFiles(const juce::Array<juce::File>& files) {
    // un seul travail : les sons arrivent dans l'ordre demande (le reechantillonnage reste multi-coeurs)
    ++loadingCount;
    sendChangeMessage();
    loader.addJob([this, files] {
        for (auto& f : files) addFileNow(f);
        --loadingCount;
        sendChangeMessage();
    });
}

// ------------------------------------------------------------------ tri

void SamplerProcessor::sortLocked(SortMode mode) {
    if (mode == SortMode::Reverse) {
        std::reverse(zones.begin(), zones.end());
        return;
    }
    std::stable_sort(zones.begin(), zones.end(), [mode](const ZoneData& a, const ZoneData& b) {
        if (mode == SortMode::ByNote) {
            if (a.settings.root != b.settings.root) return a.settings.root < b.settings.root;
            if (a.settings.low != b.settings.low) return a.settings.low < b.settings.low;
        }
        return a.file.getFileName().compareNatural(b.file.getFileName()) < 0;
    });
}

void SamplerProcessor::sortZones(SortMode mode) {
    {
        const juce::ScopedLock sl(zonesLock);
        sortLocked(mode);
    }
    if (mode != SortMode::ByNote) autoSort = false;   // l'ordre choisi n'est plus celui des notes
    publishZones();
    sendChangeMessage();
}

void SamplerProcessor::setAutoSort(bool on) {
    autoSort = on;
    if (on) sortZones(SortMode::ByNote);
    else sendChangeMessage();
}

std::vector<SamplerProcessor::ZoneInfo> SamplerProcessor::getZones() const {
    std::vector<ZoneInfo> out;
    const juce::ScopedLock sl(zonesLock);
    for (auto& z : zones) {
        juce::String info = juce::String(z.channels == 1 ? frs("mono") : frs("stéréo")) + " · "
                            + juce::String(z.originalRate / 1000.0, 1) + " kHz";
        out.push_back({ z.id, z.file, z.settings,
                        z.original ? z.original->getNumSamples() / z.originalRate : 0.0, info });
    }
    return out;
}

void SamplerProcessor::updateZone(uint32_t id, const pvoc::ZoneSettings& s) {
    {
        const juce::ScopedLock sl(zonesLock);
        for (auto& z : zones) {
            if (z.id != id) continue;
            const bool rangeEdited = s.low != z.settings.low || s.high != z.settings.high;
            const bool rootEdited = s.root != z.settings.root;
            z.settings = s;
            z.settings.low = juce::jlimit(0, 127, z.settings.low);
            z.settings.high = juce::jlimit(z.settings.low, 127, z.settings.high);
            if (rangeEdited) z.settings.autoRange = false;   // l'utilisateur a choisi sa plage
            if (rootEdited && z.settings.autoRange) {
                std::vector<ZoneData*> ptrs;
                for (auto& zz : zones) ptrs.push_back(&zz);
                pvoc::spreadRanges(ptrs, false);
            }
            if (rootEdited && autoSort.load()) { sortLocked(SortMode::ByNote); break; }
        }
    }
    publishZones();
    sendChangeMessage();
}

void SamplerProcessor::removeZone(uint32_t id) {
    {
        const juce::ScopedLock sl(zonesLock);
        zones.erase(std::remove_if(zones.begin(), zones.end(), [id](const ZoneData& z) { return z.id == id; }), zones.end());
        std::vector<ZoneData*> ptrs;
        for (auto& zz : zones) ptrs.push_back(&zz);
        pvoc::spreadRanges(ptrs, false);
    }
    publishZones();
    sendChangeMessage();
}

void SamplerProcessor::removeAllZones() {
    {
        const juce::ScopedLock sl(zonesLock);
        zones.clear();
    }
    publishZones();
    sendChangeMessage();
}

void SamplerProcessor::autoSpread() {
    {
        const juce::ScopedLock sl(zonesLock);
        std::vector<ZoneData*> ptrs;
        for (auto& zz : zones) ptrs.push_back(&zz);
        pvoc::spreadRanges(ptrs, true);
    }
    publishZones();
    sendChangeMessage();
}

juce::String SamplerProcessor::getLastError() const {
    const juce::ScopedLock sl(zonesLock);
    return lastError;
}

void SamplerProcessor::publishZones() {
    auto set = std::make_shared<pvoc::ZoneSet>();
    {
        const juce::ScopedLock sl(zonesLock);
        for (auto& z : zones) {
            if (z.buffer == nullptr) continue;
            set->zones.push_back({ z.id, z.settings, z.buffer });
        }
    }
    const juce::SpinLock::ScopedLockType sl(handoff);
    for (auto& z : set->zones) keepAlive.push_back(z.buffer);
    pendingSet = std::move(set);
    hasPending.store(true, std::memory_order_release);
}

void SamplerProcessor::timerCallback() {
    // liberation de la memoire hors du thread audio
    std::vector<std::shared_ptr<const pvoc::ZoneSet>> old;
    std::vector<std::shared_ptr<const pvoc::PlayerBuffer>> toFree;
    {
        const juce::SpinLock::ScopedLockType sl(handoff);
        for (auto& slot : retiredSets)
            if (slot != nullptr) { old.push_back(std::move(slot)); slot = nullptr; }
    }
    old.clear();
    {
        const juce::SpinLock::ScopedLockType sl(handoff);
        // un son n'est libere que lorsque plus personne (zones, voix) ne l'utilise
        std::sort(keepAlive.begin(), keepAlive.end());
        keepAlive.erase(std::unique(keepAlive.begin(), keepAlive.end()), keepAlive.end());
        for (auto it = keepAlive.begin(); it != keepAlive.end();) {
            if (it->use_count() == 1) { toFree.push_back(std::move(*it)); it = keepAlive.erase(it); }
            else ++it;
        }
    }
    toFree.clear();
}

// ------------------------------------------------------------------ MIDI et voix

void SamplerProcessor::noteOn(int note, float velocity) {
    const auto* set = currentSet.get();
    const int maxVoices = juce::jlimit(1, MAX_VOICES, (int) pVoices->load());
    const float sens = pVelocity->load() / 100.0f;
    const float velGain = (1.0f - sens) + sens * velocity * velocity;

    for (const auto& z : set->zones) {
        if (note < z.s.low || note > z.s.high) continue;
        // voix libre, sinon on vole la plus ancienne (en priorite une voix deja relachee)
        Voice* target = nullptr;
        for (int i = 0; i < maxVoices; ++i)
            if (!voices[(size_t) i].active) { target = &voices[(size_t) i]; break; }
        if (target == nullptr) {
            for (int pass = 0; pass < 2 && target == nullptr; ++pass) {
                uint64_t oldest = UINT64_MAX;
                for (int i = 0; i < maxVoices; ++i) {
                    auto& v = voices[(size_t) i];
                    if (pass == 0 && !v.releasing) continue;
                    if (v.age < oldest) { oldest = v.age; target = &v; }
                }
            }
        }
        if (target == nullptr) return;
        auto& v = *target;
        v.active = true;
        v.note = note;
        v.zoneId = z.id;
        v.s = z.s;
        v.buffer = z.buffer;
        v.keyDown = true;
        v.sustained = false;
        v.releasing = false;
        v.env = 1.0f;
        v.velGain = velGain;
        v.age = ++ageCounter;
        v.eng->setPlayerBuffer(v.buffer);
        v.eng->setScrubTime(pSmooth->load());
        applyVoiceSettings(v);
        v.penvStage = -1;
        v.penvDone = false;
        if (v.s.posEnv) {
            // la position suit l'enveloppe des le debut de la note
            v.penvStage = 0;
            v.penvT = 0.0f;
            v.lfoPhase = 0.0;
            v.lfoA = lfoRandom.nextFloat() * 2.0f - 1.0f;
            v.lfoB = lfoRandom.nextFloat() * 2.0f - 1.0f;
            v.lastEnvPos = pvoc::posEnvValue(effectiveEnv(v.s), 0, 0.0f, 0.0f);
            v.eng->seek(v.lastEnvPos);
        } else if (pFollow->load() > 0.5f) v.eng->seek(pPosition->load() / 100.0);
        else v.eng->seek(v.s.start / 100.0);
        v.eng->play();
    }
}

void SamplerProcessor::noteOff(int note) {
    for (auto& v : voices) {
        if (!v.active || v.note != note || !v.keyDown) continue;
        v.keyDown = false;
        if (sustainPedal) v.sustained = true;
        else releaseVoice(v);
    }
}

void SamplerProcessor::releaseVoice(Voice& v) {
    if (v.s.mode == pvoc::PlayMode::OneShot) return;   // joue jusqu'au bout
    if (v.penvStage >= 0) {
        // enveloppe de position : on glisse d'abord vers la position de fin,
        // le fondu de volume commencera quand elle sera atteinte
        if (v.penvStage < 3) {
            v.relFrom = v.lastEnvPos;
            v.penvStage = 3;
            v.penvT = 0.0f;
        }
        return;
    }
    startAmpRelease(v);
}

void SamplerProcessor::startAmpRelease(Voice& v) {
    if (v.releasing) return;
    v.releasing = true;
    const float samples = (float) (juce::jmax(5.0f, v.s.releaseMs) * 0.001 * currentRate);
    v.relStep = v.env / samples;
}

void SamplerProcessor::killVoice(Voice& v) {
    // fondu tres court (5 ms), utilise pour "tout arreter" ou la reduction du nombre de voix
    v.s.mode = pvoc::PlayMode::Hold;
    v.s.releaseMs = 5.0f;
    v.releasing = false;
    startAmpRelease(v);
}

pvoc::ZoneSettings SamplerProcessor::effectiveEnv(const pvoc::ZoneSettings& in) const {
    // reglages du son + reglages globaux automatisables (decalages, echelles des durees, LFO)
    auto s = in;
    float* pos[4] = { &s.pStart, &s.pPeak, &s.pSustain, &s.pEnd };
    for (int i = 0; i < 4; ++i) *pos[i] = juce::jlimit(0.0f, 100.0f, *pos[i] + envOfsCur[i]);
    s.aMs *= pEnvTime[0]->load() / 100.0f;
    s.dMs *= pEnvTime[1]->load() / 100.0f;
    s.rMs *= pEnvTime[2]->load() / 100.0f;
    s.lfoRate *= pLfoRate->load() / 100.0f;
    s.lfoDepth *= pLfoDepth->load() / 100.0f;
    return s;
}

float SamplerProcessor::positionEnvelope(Voice& v, int n) {
    const auto s = effectiveEnv(v.s);
    float pos = pvoc::posEnvValue(s, v.penvStage, v.penvT, v.relFrom);
    v.lastEnvPos = pos;

    if (s.lfo && s.lfoDepth > 0.0f) {
        const double ph = v.lfoPhase - std::floor(v.lfoPhase);
        float w;
        if (s.lfoShape == 1) w = (float) (1.0 - 4.0 * std::abs(ph - 0.5));                  // triangle
        else if (s.lfoShape == 2) {                                                           // aleatoire doux
            const float x = (float) (0.5 - 0.5 * std::cos(juce::MathConstants<double>::pi * ph));
            w = v.lfoA + (v.lfoB - v.lfoA) * x;
        } else w = (float) std::sin(2.0 * juce::MathConstants<double>::pi * ph);              // sinus
        pos += 0.01f * s.lfoDepth * w;
        const double next = v.lfoPhase + s.lfoRate * n / currentRate;
        if (std::floor(next) > std::floor(v.lfoPhase)) {   // nouveau cycle : nouvelle cible aleatoire
            v.lfoA = v.lfoB;
            v.lfoB = lfoRandom.nextFloat() * 2.0f - 1.0f;
        }
        v.lfoPhase = next;
    }

    // avance dans l'enveloppe
    v.penvT += (float) (1000.0 * n / currentRate);
    if (v.penvStage == 0 && v.penvT >= s.aMs) { v.penvT -= s.aMs; v.penvStage = 1; }
    if (v.penvStage == 1 && v.penvT >= s.dMs) {
        v.penvT -= s.dMs;
        if (s.mode == pvoc::PlayMode::OneShot) {   // one-shot : pas de maintien, on enchaine sur R
            v.penvStage = 3;
            v.relFrom = 0.01f * s.pSustain;
        } else {
            v.penvStage = 2;
        }
    }
    if (v.penvStage == 3 && !v.penvDone && v.penvT >= s.rMs) {
        v.penvDone = true;   // position de fin atteinte : fondu de volume
        startAmpRelease(v);
    }
    return juce::jlimit(0.0f, 1.0f, pos);
}

void SamplerProcessor::handleMidi(const juce::MidiMessage& m) {
    if (m.isNoteOn()) noteOn(m.getNoteNumber(), m.getFloatVelocity());
    else if (m.isNoteOff()) noteOff(m.getNoteNumber());
    else if (m.isSustainPedalOn()) sustainPedal = true;
    else if (m.isSustainPedalOff()) {
        sustainPedal = false;
        for (auto& v : voices)
            if (v.active && v.sustained) { v.sustained = false; releaseVoice(v); }
    } else if (m.isPitchWheel()) {
        pitchBend = (float) (m.getPitchWheelValue() - 8192) / 8192.0f * pBendRange->load();
    } else if (m.isAllNotesOff() || m.isAllSoundOff()) {
        for (auto& v : voices) if (v.active) killVoice(v);
        sustainPedal = false;
    }
}

void SamplerProcessor::applyVoiceSettings(Voice& v) {
    auto& e = *v.eng;
    const double speed = (pSpeed->load() / 100.0) * (v.s.speed / 100.0);
    e.setSpeed(speed);
    e.setTranspose((double) (v.note - v.s.root) + v.s.tuneCents / 100.0 + pTranspose->load() + pitchBend);
    e.setLock(pLock->load() > 0.5f);
    e.setLoop(v.s.loop);
    e.setGain(juce::Decibels::decibelsToGain(v.s.gainDb, -60.0f) * v.velGain);
}

void SamplerProcessor::renderVoices(float* outL, float* outR, int n) {
    // decalages de l'enveloppe lisses (~30 ms) : un bouton MIDI tourne vite ne fait pas de sauts
    const float k = 1.0f - std::exp(-(float) n / (0.030f * (float) currentRate));
    for (int i = 0; i < 4; ++i) envOfsCur[i] += (pEnvOfs[i]->load() - envOfsCur[i]) * k;
    const bool follow = pFollow->load() > 0.5f;
    const double pos = pPosition->load() / 100.0;
    const float smooth = pSmooth->load();
    for (auto& v : voices) {
        if (!v.active) continue;
        // reglages de la zone modifies pendant que la note sonne
        if (const auto* z = currentSet->find(v.zoneId)) {
            const auto keepMode = v.s.mode;
            const auto keepRel = v.s.releaseMs;
            v.s = z->s;
            if (v.releasing) { v.s.mode = keepMode; v.s.releaseMs = keepRel; }
        }
        applyVoiceSettings(v);
        if (!v.s.posEnv) v.penvStage = -1;   // enveloppe desactivee pendant la note
        if (v.penvStage >= 0) {
            v.eng->setScrubTime(0.0);           // l'enveloppe est deja continue
            v.eng->setScrub(true, positionEnvelope(v, n));
        } else {
            v.eng->setScrubTime(smooth);
            v.eng->setScrub(follow, pos);
        }

        for (int p = 0; p < n;) {
            const int k = juce::jmin(n - p, (int) tmpL.size());
            v.eng->process(nullptr, nullptr, tmpL.data(), tmpR.data(), k);
            for (int i = 0; i < k; ++i) {
                float g = v.env;
                if (v.releasing) {
                    v.env = juce::jmax(0.0f, v.env - v.relStep);
                    g = v.env;
                }
                outL[p + i] += tmpL[(size_t) i] * g;
                outR[p + i] += tmpR[(size_t) i] * g;
            }
            p += k;
        }
        v.eng->takeEndFlag();
        // fin : fondu de relachement termine, ou son arrive au bout (le moteur a deja fait son fondu)
        if ((v.releasing && v.env <= 0.0f) || !v.eng->isSounding()) {
            v.active = false;
            v.eng->pause();
            v.buffer.reset();   // le son reste reference par keepAlive : pas de liberation ici
        }
    }
}

void SamplerProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    buffer.clear();

    if (hasPending.load(std::memory_order_acquire)) {
        const juce::SpinLock::ScopedTryLockType tl(handoff);
        if (tl.isLocked() && pendingSet != nullptr) {
            for (auto& slot : retiredSets) {
                if (slot != nullptr) continue;
                slot = std::move(currentSet);           // l'ancienne version sera liberee par le timer
                currentSet = std::move(pendingSet);
                pendingSet = nullptr;
                hasPending = false;
                break;
            }
        }
    }

    // nombre de voix reduit : on eteint les voix en trop
    const int maxVoices = juce::jlimit(1, MAX_VOICES, (int) pVoices->load());
    for (int i = maxVoices; i < MAX_VOICES; ++i)
        if (voices[(size_t) i].active && !voices[(size_t) i].releasing) killVoice(voices[(size_t) i]);

    keyboardState.processNextMidiBuffer(midi, 0, n, true);

    float* outL = buffer.getWritePointer(0);
    float* outR = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;
    float dummyR[512];

    int pos = 0;
    auto renderTo = [&](int end) {
        while (pos < end) {
            const int k = juce::jmin(end - pos, 512);
            if (outR != nullptr) renderVoices(outL + pos, outR + pos, k);
            else {
                std::fill(dummyR, dummyR + k, 0.0f);
                renderVoices(outL + pos, dummyR, k);
                for (int i = 0; i < k; ++i) outL[pos + i] = 0.5f * (outL[pos + i] + dummyR[i]);
            }
            pos += k;
        }
    };
    for (const auto meta : midi) {
        renderTo(juce::jlimit(0, n, meta.samplePosition));
        handleMidi(meta.getMessage());
    }
    renderTo(n);

    masterGain.setTargetValue(pGain->load() <= -59.9f ? 0.0f : juce::Decibels::decibelsToGain(pGain->load()));
    for (int i = 0; i < n; ++i) {
        const float g = masterGain.getNextValue();
        outL[i] *= g;
        if (outR != nullptr) outR[i] *= g;
    }
    for (int c = 2; c < buffer.getNumChannels(); ++c) buffer.clear(c, 0, n);

    int count = 0;
    uint64_t lo = 0, hi = 0;
    for (auto& v : voices)
        if (v.active) {
            ++count;
            if (v.note < 64) lo |= (uint64_t) 1 << v.note;
            else hi |= (uint64_t) 1 << (v.note - 64);
        }
    activeVoices = count;
    soundingLo = lo;
    soundingHi = hi;
}

// ------------------------------------------------------------------ etat

juce::ValueTree SamplerProcessor::createStateTree(const juce::File* relativeTo) {
    auto state = params.copyState();
    juce::ValueTree zt("Zones");
    {
        const juce::ScopedLock sl(zonesLock);
        for (auto& z : zones) {
            juce::ValueTree t("Zone");
            if (relativeTo != nullptr && z.file.isAChildOf(*relativeTo)) {
                t.setProperty("file", z.file.getRelativePathFrom(*relativeTo).replaceCharacter('\\', '/'), nullptr);
                t.setProperty("rel", true, nullptr);
            } else {
                t.setProperty("file", z.file.getFullPathName(), nullptr);
            }
            const auto& s = z.settings;
            t.setProperty("root", s.root, nullptr);
            t.setProperty("low", s.low, nullptr);
            t.setProperty("high", s.high, nullptr);
            t.setProperty("mode", (int) s.mode, nullptr);
            t.setProperty("loop", s.loop, nullptr);
            t.setProperty("release", s.releaseMs, nullptr);
            t.setProperty("gain", s.gainDb, nullptr);
            t.setProperty("speed", s.speed, nullptr);
            t.setProperty("start", s.start, nullptr);
            t.setProperty("tune", s.tuneCents, nullptr);
            t.setProperty("auto", s.autoRange, nullptr);
            t.setProperty("penv", s.posEnv, nullptr);
            t.setProperty("pStart", s.pStart, nullptr);
            t.setProperty("pPeak", s.pPeak, nullptr);
            t.setProperty("pSustain", s.pSustain, nullptr);
            t.setProperty("pEnd", s.pEnd, nullptr);
            t.setProperty("aMs", s.aMs, nullptr);
            t.setProperty("dMs", s.dMs, nullptr);
            t.setProperty("rMs", s.rMs, nullptr);
            t.setProperty("lfo", s.lfo, nullptr);
            t.setProperty("lfoShape", s.lfoShape, nullptr);
            t.setProperty("lfoRate", s.lfoRate, nullptr);
            t.setProperty("lfoDepth", s.lfoDepth, nullptr);
            zt.appendChild(t, nullptr);
        }
        state.setProperty("bank", currentBank.getFullPathName(), nullptr);
    }
    state.setProperty("autoSort", autoSort.load(), nullptr);
    state.removeChild(state.getChildWithName("Zones"), nullptr);
    state.appendChild(zt, nullptr);
    return state;
}

void SamplerProcessor::getStateInformation(juce::MemoryBlock& destData) {
    if (auto xml = createStateTree(nullptr).createXml()) copyXmlToBinary(*xml, destData);
}

void SamplerProcessor::setStateInformation(const void* data, int sizeInBytes) {
    auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr || !xml->hasTagName(params.state.getType())) return;
    applyStateTree(juce::ValueTree::fromXml(*xml), {});
}

void SamplerProcessor::applyStateTree(juce::ValueTree state, const juce::File& baseDir) {
    const auto zt = state.getChildWithName("Zones");
    state.removeChild(zt, nullptr);
    autoSort = (bool) state.getProperty("autoSort", true);
    {
        const juce::ScopedLock sl(zonesLock);
        const juce::String bank = state.getProperty("bank", juce::String());
        if (baseDir == juce::File()) currentBank = bank.isNotEmpty() ? juce::File(bank) : juce::File();
    }
    params.replaceState(state);

    std::vector<std::pair<juce::File, pvoc::ZoneSettings>> wanted;
    for (int i = 0; i < zt.getNumChildren(); ++i) {
        const auto t = zt.getChild(i);
        pvoc::ZoneSettings s;
        s.root = t.getProperty("root", 60);
        s.low = t.getProperty("low", 0);
        s.high = t.getProperty("high", 127);
        s.mode = (int) t.getProperty("mode", 0) == 1 ? pvoc::PlayMode::OneShot : pvoc::PlayMode::Hold;
        s.loop = t.getProperty("loop", false);
        s.releaseMs = t.getProperty("release", 200.0f);
        s.gainDb = t.getProperty("gain", 0.0f);
        s.speed = t.getProperty("speed", 100.0f);
        s.start = t.getProperty("start", 0.0f);
        s.tuneCents = t.getProperty("tune", 0.0f);
        s.autoRange = t.getProperty("auto", true);
        s.posEnv = t.getProperty("penv", false);
        s.pStart = t.getProperty("pStart", 0.0f);
        s.pPeak = t.getProperty("pPeak", 50.0f);
        s.pSustain = t.getProperty("pSustain", 50.0f);
        s.pEnd = t.getProperty("pEnd", 100.0f);
        s.aMs = t.getProperty("aMs", 500.0f);
        s.dMs = t.getProperty("dMs", 500.0f);
        s.rMs = t.getProperty("rMs", 1000.0f);
        s.lfo = t.getProperty("lfo", false);
        s.lfoShape = t.getProperty("lfoShape", 0);
        s.lfoRate = t.getProperty("lfoRate", 0.5f);
        s.lfoDepth = t.getProperty("lfoDepth", 3.0f);
        const juce::String path = t.getProperty("file").toString();
        const bool rel = t.getProperty("rel", false);
        const juce::File f = rel ? baseDir.getChildFile(path) : juce::File(path);
        wanted.push_back({ f, s });
    }
    // meme liste de sons que celle deja chargee : on ne recharge rien, on met juste a jour les reglages
    bool same = false;
    {
        const juce::ScopedLock sl(zonesLock);
        same = zones.size() == wanted.size();
        for (size_t i = 0; same && i < zones.size(); ++i) same = zones[i].file == wanted[i].first;
        if (same)
            for (size_t i = 0; i < zones.size(); ++i) zones[i].settings = wanted[i].second;
    }
    if (same) {
        publishZones();
        sendChangeMessage();
        return;
    }
    removeAllZones();
    ++loadingCount;
    loader.addJob([this, wanted] {
        juce::StringArray missing;
        restoring = true;   // l'ordre enregistre est respecte (pas de tri automatique)
        for (auto& w : wanted) {
            if (!w.first.existsAsFile()) { missing.add(w.first.getFileName()); continue; }
            const uint32_t id = addFileNow(w.first, w.second.root);
            if (id != 0) {
                const juce::ScopedLock sl(zonesLock);
                for (auto& z : zones) if (z.id == id) z.settings = w.second;
            }
        }
        restoring = false;
        if (!missing.isEmpty()) {
            const juce::ScopedLock sl(zonesLock);
            lastError = frs("Sons introuvables : ") + missing.joinIntoString(", ");
        }
        publishZones();
        --loadingCount;
        sendChangeMessage();
    });
}

// ------------------------------------------------------------------ banques de sons

juce::File SamplerProcessor::getBanksFolder() {
    auto d = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Sampler PVoc").getChildFile("Banques");
    d.createDirectory();
    return d;
}

juce::Array<juce::File> SamplerProcessor::listBanks() {
    auto files = getBanksFolder().findChildFiles(juce::File::findFiles, true, "*.pvocbank");
    std::sort(files.begin(), files.end(), [](const juce::File& a, const juce::File& b) {
        return a.getFileNameWithoutExtension().compareNatural(b.getFileNameWithoutExtension()) < 0;
    });
    return files;
}

juce::String SamplerProcessor::saveBankNow(const juce::File& bankFileIn, bool copySounds) {
    const juce::File bankFile = bankFileIn.withFileExtension("pvocbank");
    const juce::File dir = bankFile.getParentDirectory();
    if (!dir.createDirectory()) return frs("Impossible de créer le dossier ") + dir.getFullPathName();

    if (copySounds) {
        // copie des sons dans "<nom> (sons)", a cote de la banque
        const juce::File soundsDir = dir.getChildFile(bankFile.getFileNameWithoutExtension() + " (sons)");
        soundsDir.createDirectory();
        std::vector<std::pair<uint32_t, juce::File>> todo;
        {
            const juce::ScopedLock sl(zonesLock);
            for (auto& z : zones) todo.push_back({ z.id, z.file });
        }
        std::map<juce::String, juce::File> done;   // un meme fichier n'est copie qu'une fois
        for (auto& [id, src] : todo) {
            if (src.isAChildOf(soundsDir)) continue;
            juce::File dst;
            const auto key = src.getFullPathName();
            if (done.count(key)) dst = done[key];
            else {
                dst = soundsDir.getChildFile(src.getFileName());
                for (int n = 2; dst.existsAsFile() && dst.getSize() != src.getSize(); ++n)
                    dst = soundsDir.getChildFile(src.getFileNameWithoutExtension() + " (" + juce::String(n) + ")" + src.getFileExtension());
                if (!dst.existsAsFile() && !src.copyFileTo(dst)) return frs("Impossible de copier ") + src.getFileName();
                done[key] = dst;
            }
            const juce::ScopedLock sl(zonesLock);
            for (auto& z : zones) if (z.id == id) z.file = dst;   // le projet utilise desormais la copie
        }
    }

    {
        const juce::ScopedLock sl(zonesLock);
        currentBank = bankFile;
    }
    juce::ValueTree root("SamplerPVocBank");
    root.setProperty("version", 1, nullptr);
    root.appendChild(createStateTree(&dir), nullptr);
    auto xml = root.createXml();
    if (xml == nullptr || !xml->writeTo(bankFile)) return frs("Impossible d'écrire ") + bankFile.getFullPathName();
    {
        const juce::ScopedLock sl(zonesLock);
        infoMessage = frs("Banque « ") + bankFile.getFileNameWithoutExtension() + frs(" » enregistrée");
        lastError.clear();
    }
    return {};
}

void SamplerProcessor::saveBank(const juce::File& bankFile, bool copySounds) {
    ++loadingCount;
    sendChangeMessage();
    loader.addJob([this, bankFile, copySounds] {
        const auto err = saveBankNow(bankFile, copySounds);
        if (err.isNotEmpty()) {
            const juce::ScopedLock sl(zonesLock);
            lastError = err;
        }
        --loadingCount;
        sendChangeMessage();
    });
}

juce::String SamplerProcessor::loadBank(const juce::File& bankFile) {
    auto xml = juce::XmlDocument::parse(bankFile);
    if (xml == nullptr || !xml->hasTagName("SamplerPVocBank")) return frs("Ce fichier n'est pas une banque Sampler PVoc.");
    auto root = juce::ValueTree::fromXml(*xml);
    auto state = root.getChildWithName(params.state.getType());
    if (!state.isValid()) return frs("Banque illisible.");
    {
        const juce::ScopedLock sl(zonesLock);
        currentBank = bankFile;
        lastError.clear();
        infoMessage = frs("Banque « ") + bankFile.getFileNameWithoutExtension() + frs(" » ouverte");
    }
    applyStateTree(state.createCopy(), bankFile.getParentDirectory());
    return {};
}

juce::File SamplerProcessor::getCurrentBank() const {
    const juce::ScopedLock sl(zonesLock);
    return currentBank;
}

juce::String SamplerProcessor::getInfoMessage() const {
    const juce::ScopedLock sl(zonesLock);
    return infoMessage;
}

juce::AudioProcessorEditor* SamplerProcessor::createEditor() { return new SamplerEditor(*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SamplerProcessor(); }
