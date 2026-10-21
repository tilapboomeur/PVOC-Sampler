#include "SamplerProcessor.h"
#include "SamplerEditor.h"
#include "../Resampler.h"

#include <cmath>
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
        lastError.clear();
    }
    publishZones();
    sendChangeMessage();
    return id;
}

void SamplerProcessor::addFiles(const juce::Array<juce::File>& files) {
    for (auto& f : files) {
        ++loadingCount;
        sendChangeMessage();
        loader.addJob([this, f] {
            addFileNow(f);
            --loadingCount;
            sendChangeMessage();
        });
    }
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
        if (pFollow->load() > 0.5f) v.eng->seek(pPosition->load() / 100.0);
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
    releaseVoice(v);
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
        v.eng->setScrubTime(smooth);
        v.eng->setScrub(follow, pos);

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

void SamplerProcessor::getStateInformation(juce::MemoryBlock& destData) {
    auto state = params.copyState();
    juce::ValueTree zt("Zones");
    {
        const juce::ScopedLock sl(zonesLock);
        for (auto& z : zones) {
            juce::ValueTree t("Zone");
            t.setProperty("file", z.file.getFullPathName(), nullptr);
            t.setProperty("root", z.settings.root, nullptr);
            t.setProperty("low", z.settings.low, nullptr);
            t.setProperty("high", z.settings.high, nullptr);
            t.setProperty("mode", (int) z.settings.mode, nullptr);
            t.setProperty("loop", z.settings.loop, nullptr);
            t.setProperty("release", z.settings.releaseMs, nullptr);
            t.setProperty("gain", z.settings.gainDb, nullptr);
            t.setProperty("speed", z.settings.speed, nullptr);
            t.setProperty("start", z.settings.start, nullptr);
            t.setProperty("tune", z.settings.tuneCents, nullptr);
            t.setProperty("auto", z.settings.autoRange, nullptr);
            zt.appendChild(t, nullptr);
        }
    }
    state.removeChild(state.getChildWithName("Zones"), nullptr);
    state.appendChild(zt, nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary(*xml, destData);
}

void SamplerProcessor::setStateInformation(const void* data, int sizeInBytes) {
    auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr || !xml->hasTagName(params.state.getType())) return;
    auto state = juce::ValueTree::fromXml(*xml);
    const auto zt = state.getChildWithName("Zones");
    state.removeChild(zt, nullptr);
    params.replaceState(state);

    // meme liste de sons que celle deja chargee : on ne recharge rien, on met juste a jour les reglages
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
        wanted.push_back({ juce::File(t.getProperty("file").toString()), s });
    }
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
        for (auto& w : wanted) {
            if (!w.first.existsAsFile()) { missing.add(w.first.getFileName()); continue; }
            const uint32_t id = addFileNow(w.first, w.second.root);
            if (id != 0) {
                const juce::ScopedLock sl(zonesLock);
                for (auto& z : zones) if (z.id == id) z.settings = w.second;
            }
        }
        if (!missing.isEmpty()) {
            const juce::ScopedLock sl(zonesLock);
            lastError = frs("Sons introuvables : ") + missing.joinIntoString(", ");
        }
        publishZones();
        --loadingCount;
        sendChangeMessage();
    });
}

juce::AudioProcessorEditor* SamplerProcessor::createEditor() { return new SamplerEditor(*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SamplerProcessor(); }
