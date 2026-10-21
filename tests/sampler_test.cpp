// Tests du Sampler PVoc (processeur complet + interface), sans hote.
#include "../Source/Sampler/SamplerProcessor.h"
#include "../Source/Sampler/SamplerEditor.h"

#include <cmath>
#include <cstdio>

static int failures = 0;
static void check(bool ok, const juce::String& what) {
    std::printf("%s %s\n", ok ? "[OK]  " : "[ECHEC]", what.toRawUTF8());
    if (!ok) ++failures;
}

static const double sr = 48000.0;

static juce::File makeSine(const juce::String& name, double freq, double seconds, double rate) {
    const auto f = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile(name);
    f.deleteFile();
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream>(f);
    auto w = fmt.createWriterFor(os, juce::AudioFormatWriterOptions {}.withSampleRate(rate).withNumChannels(2).withBitsPerSample(24));
    const int n = (int) (seconds * rate);
    juce::AudioBuffer<float> b(2, n);
    for (int i = 0; i < n; ++i) {
        const float v = 0.5f * (float) std::sin(2 * juce::MathConstants<double>::pi * freq * i / rate);
        b.setSample(0, i, v);
        b.setSample(1, i, v);
    }
    w->writeFromAudioSampleBuffer(b, 0, n);
    return f;
}

struct Rig {
    SamplerProcessor p;
    std::vector<float> out;
    Rig() {
        p.setPlayConfigDetails(0, 2, sr, 512);
        p.prepareToPlay(sr, 512);
    }
    /** Joue n echantillons ; events = (position, message). */
    std::vector<float> run(int n, std::vector<std::pair<int, juce::MidiMessage>> events = {}) {
        std::vector<float> o;
        for (int pos = 0; pos < n; pos += 512) {
            const int k = std::min(512, n - pos);
            juce::AudioBuffer<float> b(2, k);
            juce::MidiBuffer m;
            for (auto& e : events)
                if (e.first >= pos && e.first < pos + k) m.addEvent(e.second, e.first - pos);
            p.processBlock(b, m);
            for (int i = 0; i < k; ++i) o.push_back(b.getSample(0, i));
        }
        return o;
    }
    void set(const char* id, float v) {
        auto* prm = p.params.getParameter(id);
        prm->setValueNotifyingHost(prm->convertTo0to1(v));
    }
};

static double goertzel(const std::vector<float>& x, size_t a, size_t b, double f) {
    double re = 0, im = 0;
    for (size_t i = a; i < b; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2 * juce::MathConstants<double>::pi * (double) (i - a) / (double) (b - a));
        re += x[i] * w * std::cos(2 * juce::MathConstants<double>::pi * f * (double) i / sr);
        im += x[i] * w * std::sin(2 * juce::MathConstants<double>::pi * f * (double) i / sr);
    }
    return 4.0 * std::sqrt(re * re + im * im) / (double) (b - a);   // ~ amplitude
}

static double freqOf(const std::vector<float>& x, size_t a, size_t b) {
    double first = -1, last = -1;
    int cnt = 0;
    for (size_t i = a + 1; i < b; ++i)
        if (x[i - 1] < 0 && x[i] >= 0) {
            const double t = (double) i - 1 + x[i - 1] / (x[i - 1] - x[i]);
            if (first < 0) first = t;
            last = t;
            ++cnt;
        }
    return cnt > 1 ? (cnt - 1) * sr / (last - first) : 0.0;
}

static double peak(const std::vector<float>& x, size_t a, size_t b) {
    double s = 0;
    for (size_t i = a; i < b; ++i) s += x[i] * x[i];
    return std::sqrt(2.0 * s / (double) (b - a));
}

static juce::MidiMessage on(int n, float v = 1.0f) { return juce::MidiMessage::noteOn(1, n, v); }
static juce::MidiMessage off(int n) { return juce::MidiMessage::noteOff(1, n); }

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI gui;

    // noms de notes
    check(pvoc::noteFromFileName("Sinus_A3.wav") == 69, "nom de fichier : Sinus_A3 -> A3 (69)");
    check(pvoc::noteFromFileName("Voix Do3.wav") == 60, "nom de fichier : Voix Do3 -> Do3 (60)");
    check(pvoc::noteFromFileName(juce::String::fromUTF8("basse_r\xc3\xa9#1.wav")) == 39, "nom de fichier : basse_ré#1 -> 39");
    check(pvoc::noteFromFileName("Flute F#2 take2.wav") == 54, "nom de fichier : Flute F#2 -> 54");
    check(pvoc::noteFromFileName("Ambiance.wav") == -1, "nom de fichier sans note -> aucune");
    check(pvoc::noteName(60) == "C3" && pvoc::noteNameFr(60) == "Do3", "noms : 60 = C3 = Do3");

    const auto aigu = makeSine("Sinus_A3.wav", 440.0, 2.0, 44100.0);
    const auto grave = makeSine("Basse_A1.wav", 110.0, 2.0, 44100.0);

    Rig r;
    r.set("gain", 0.0f);
    r.set("velocity", 0.0f);
    const uint32_t idA = r.p.addFileNow(aigu);
    const uint32_t idB = r.p.addFileNow(grave);
    check(idA != 0 && idB != 0, "chargement de deux sons (44,1 kHz -> 48 kHz)");
    auto zs = r.p.getZones();
    check(zs.size() == 2 && zs[0].settings.root == 69 && zs[1].settings.root == 45, "notes de base lues dans les noms (A3, A1)");
    check(zs[1].settings.low == 0 && zs[1].settings.high == 57 && zs[0].settings.low == 58 && zs[0].settings.high == 127,
          "plages reparties automatiquement : A1 0-57, A3 58-127");
    r.run(1024);

    // 1) transposition par la note, durée conservée
    {
        auto o = r.run((int) (sr * 1.0), { { 0, on(81) } });
        const double f = freqOf(o, 9600, o.size());
        check(std::abs(f - 880.0) < 0.5, "A4 sur le son A3 : " + juce::String(f, 2) + " Hz (attendu 880)");
        check(std::abs(peak(o, 9600, o.size()) - 0.5) < 0.02, "niveau " + juce::String(peak(o, 9600, o.size()), 3));
        r.run((int) sr, { { 0, off(81) } });
    }
    // 2) bonne zone : D2 (50) -> son grave transposé de +5
    {
        auto o = r.run((int) (sr * 1.0), { { 0, on(50) } });
        const double f = freqOf(o, 9600, o.size());
        check(std::abs(f - 110.0 * std::pow(2.0, 5.0 / 12.0)) < 0.5, "D2 sur la zone grave : " + juce::String(f, 2) + " Hz (attendu 146,83)");
        // 3) mode tenu : silence apres le relachement (200 ms)
        auto o2 = r.run((int) (sr * 0.6), { { 0, off(50) } });
        check(peak(o2, (size_t) (sr * 0.35), o2.size()) < 1e-3, "mode tenu : silence apres le relachement");
        check(r.p.getActiveVoices() == 0, "voix liberee");
    }
    // 4) polyphonie : accord de 3 notes avec 2 voix -> la plus ancienne est volee
    {
        r.set("voices", 2.0f);
        auto o = r.run((int) (sr * 1.0), { { 0, on(69) }, { 4800, on(73) }, { 9600, on(76) } });
        const double f69 = goertzel(o, 24000, o.size(), 440.0), f73 = goertzel(o, 24000, o.size(), 440.0 * std::pow(2.0, 4.0 / 12.0)),
                     f76 = goertzel(o, 24000, o.size(), 440.0 * std::pow(2.0, 7.0 / 12.0));
        check(r.p.getActiveVoices() == 2, "2 voix : " + juce::String(r.p.getActiveVoices()) + " voix actives");
        check(f73 > 0.4 && f76 > 0.4 && f69 < 0.02, "2 voix : la 1re note est remplacee (" + juce::String(f69, 3) + " / "
                                                        + juce::String(f73, 3) + " / " + juce::String(f76, 3) + ")");
        r.run((int) sr, { { 0, off(69) }, { 0, off(73) }, { 0, off(76) } });
        r.set("voices", 16.0f);
        o = r.run((int) (sr * 1.0), { { 0, on(69) }, { 0, on(73) }, { 0, on(76) }, { 0, on(81) } });
        check(r.p.getActiveVoices() == 4 && goertzel(o, 12000, o.size(), 440.0) > 0.4 && goertzel(o, 12000, o.size(), 880.0) > 0.4,
              "16 voix : accord de 4 notes complet");
        r.run((int) sr, { { 0, off(69) }, { 0, off(73) }, { 0, off(76) }, { 0, off(81) } });
        r.set("voices", 1.0f);
        r.run((int) (sr * 0.5), { { 0, on(69) }, { 2400, on(76) } });
        check(r.p.getActiveVoices() == 1, "mono : une seule voix");
        r.run((int) sr, { { 0, off(69) }, { 0, off(76) } });
        r.set("voices", 8.0f);
    }
    // 5) one-shot : continue apres le relachement puis s'arrete a la fin du son (2 s)
    {
        auto s = r.p.getZones()[0].settings;
        s.mode = pvoc::PlayMode::OneShot;
        r.p.updateZone(idA, s);
        r.run(1024);
        auto o = r.run((int) (sr * 3.0), { { 0, on(69) }, { 2400, off(69) } });
        check(peak(o, (size_t) (sr * 0.5), (size_t) (sr * 1.5)) > 0.45, "one-shot : joue apres le relachement");
        check(peak(o, (size_t) (sr * 2.3), o.size()) < 1e-3 && r.p.getActiveVoices() == 0, "one-shot : s'arrete a la fin du son");
        s.mode = pvoc::PlayMode::Hold;
        r.p.updateZone(idA, s);
        r.run(1024);
    }
    // 6) pedale de sustain
    {
        auto o = r.run((int) (sr * 1.0), { { 0, juce::MidiMessage::controllerEvent(1, 64, 127) }, { 0, on(69) }, { 2400, off(69) } });
        check(peak(o, (size_t) (sr * 0.5), o.size()) > 0.45, "sustain : la note tient apres le relachement");
        o = r.run((int) (sr * 0.6), { { 0, juce::MidiMessage::controllerEvent(1, 64, 0) } });
        check(peak(o, (size_t) (sr * 0.35), o.size()) < 1e-3, "sustain relache : la note s'eteint");
    }
    // 7) pitch bend (+2 demi-tons) et velocite
    {
        auto o = r.run((int) sr, { { 0, juce::MidiMessage::pitchWheel(1, 16383) }, { 0, on(69) } });
        const double f = freqOf(o, 12000, o.size());
        check(std::abs(f - 440.0 * std::pow(2.0, 2.0 * 8191.0 / 8192.0 / 12.0)) < 0.5, "pitch bend +2 : " + juce::String(f, 2) + " Hz");
        r.run((int) sr, { { 0, off(69) }, { 0, juce::MidiMessage::pitchWheel(1, 8192) } });
        r.set("velocity", 100.0f);
        o = r.run((int) sr, { { 0, on(69, 0.5f) } });
        check(std::abs(peak(o, 12000, o.size()) - 0.125) < 0.01, "velocite 64 (sensibilite 100 %) : niveau " + juce::String(peak(o, 12000, o.size()), 3));
        r.run((int) sr, { { 0, off(69) } });
        r.set("velocity", 0.0f);
    }
    // 8) vitesse globale : 50 % -> le son de 2 s dure 4 s en one-shot
    {
        auto s = r.p.getZones()[0].settings;
        s.mode = pvoc::PlayMode::OneShot;
        r.p.updateZone(idA, s);
        r.set("speed", 50.0f);
        r.run(1024);
        auto o = r.run((int) (sr * 5.0), { { 0, on(69) }, { 100, off(69) } });
        check(peak(o, (size_t) (sr * 3.5), (size_t) (sr * 3.8)) > 0.4 && peak(o, (size_t) (sr * 4.3), o.size()) < 1e-3,
              "vitesse 50 % : le son de 2 s dure 4 s, a la meme hauteur (" + juce::String(freqOf(o, 24000, 96000), 1) + " Hz)");
        r.set("speed", 100.0f);
        s.mode = pvoc::PlayMode::Hold;
        r.p.updateZone(idA, s);
        r.run(1024);
    }
    // 9) sauvegarde / restauration
    {
        auto s = r.p.getZones()[1].settings;
        s.loop = true;
        s.releaseMs = 777.0f;
        r.p.updateZone(idB, s);
        juce::MemoryBlock st;
        r.p.getStateInformation(st);
        Rig q;
        q.p.setStateInformation(st.getData(), (int) st.getSize());
        for (int i = 0; i < 500 && (q.p.isLoading() || q.p.getZones().size() < 2); ++i) juce::Thread::sleep(10);
        juce::Thread::sleep(50);
        auto qz = q.p.getZones();
        check(qz.size() == 2 && qz[1].settings.loop && std::abs(qz[1].settings.releaseMs - 777.0f) < 0.01f
                  && qz[1].settings.root == 45 && qz[0].settings.low == 58,
              "etat : sons et reglages des zones restaures");
        q.run(1024);
        auto o = q.run((int) sr, { { 0, on(81) } });
        check(std::abs(freqOf(o, 9600, o.size()) - 880.0) < 0.5, "etat : le sampler restaure joue juste");
    }

    // charge processeur : 16 voix pendant 10 s de son (information, pas un test)
    {
        r.set("voices", 16.0f);
        auto s = r.p.getZones()[0].settings;
        s.loop = true;
        r.p.updateZone(idA, s);
        r.run(1024);
        std::vector<std::pair<int, juce::MidiMessage>> ev;
        for (int k = 0; k < 16; ++k) ev.push_back({ 0, on(60 + k) });
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        r.run((int) (sr * 10.0), ev);
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        std::printf("[INFO]  16 voix : %.0f ms de calcul pour 10 s de son (%.1f %% d'un coeur)\n", ms, ms / 100.0);
        std::vector<std::pair<int, juce::MidiMessage>> offs;
        for (int k = 0; k < 16; ++k) offs.push_back({ 0, off(60 + k) });
        r.run((int) sr, offs);
        s.loop = false;
        r.p.updateZone(idA, s);
        r.run(1024);
    }

    // 10) interface
    if (argc > 1) {
        r.set("voices", 8.0f);
        r.run(4096, { { 0, on(69) }, { 0, on(73) }, { 0, on(76) } });
        r.p.keyboardState.noteOn(1, 69, 1.0f);
        r.p.keyboardState.noteOn(1, 73, 1.0f);
        r.p.keyboardState.noteOn(1, 76, 1.0f);
        std::unique_ptr<juce::AudioProcessorEditor> ed(r.p.createEditor());
        ed->setVisible(true);
        static_cast<SamplerEditor*>(ed.get())->selectRow(0);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
        auto img = ed->createComponentSnapshot(ed->getLocalBounds(), true, 2.0f);
        juce::File png(argv[1]);
        png.deleteFile();
        juce::FileOutputStream os(png);
        juce::PNGImageFormat().writeImageToStream(img, os);
        check(png.getSize() > 0, "interface : capture " + png.getFullPathName());
    }

    std::printf("\n%s (%d echec(s))\n", failures == 0 ? "TOUS LES TESTS DU SAMPLER PASSENT" : "DES TESTS ECHOUENT", failures);
    return failures == 0 ? 0 : 1;
}
