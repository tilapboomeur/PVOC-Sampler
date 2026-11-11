// Test du plugin Effet PVoc complet (bus, sidechain, MIDI, etat, interface), sans hote.
#include "../Source/Effect/EffectProcessor.h"
#include "../Source/Effect/EffectEditor.h"

#include <cmath>
#include <cstdio>
#include <functional>

static int failures = 0;
static void check(bool ok, const juce::String& what) {
    std::printf("%s %s\n", ok ? "[OK]  " : "[ECHEC]", what.toRawUTF8());
    if (!ok) ++failures;
}
static const double sr = 48000.0;
static const double PI = juce::MathConstants<double>::pi;
using Sig = std::function<float(long)>;

struct Rig {
    EffectProcessor p;
    long t = 0;
    Rig() {
        juce::AudioProcessor::BusesLayout l;
        l.inputBuses.add(juce::AudioChannelSet::stereo());
        l.inputBuses.add(juce::AudioChannelSet::stereo());   // sidechain actif
        l.outputBuses.add(juce::AudioChannelSet::stereo());
        p.setBusesLayout(l);
        p.prepareToPlay(sr, 512);
    }
    void set(const char* id, float v) {
        auto* prm = p.params.getParameter(id);
        prm->setValueNotifyingHost(prm->convertTo0to1(v));
    }
    std::vector<float> run(long n, Sig in, Sig sc = nullptr, std::vector<juce::MidiMessage> atStart = {}) {
        std::vector<float> out;
        bool first = true;
        for (long pos = 0; pos < n; pos += 512) {
            const int k = (int) std::min<long>(512, n - pos);
            juce::AudioBuffer<float> b(4, k);
            for (int i = 0; i < k; ++i) {
                const float x = in ? in(t + i) : 0.0f, s = sc ? sc(t + i) : 0.0f;
                b.setSample(0, i, x); b.setSample(1, i, x); b.setSample(2, i, s); b.setSample(3, i, s);
            }
            juce::MidiBuffer m;
            if (first) for (auto& e : atStart) m.addEvent(e, 0);
            first = false;
            p.processBlock(b, m);
            for (int i = 0; i < k; ++i) out.push_back(b.getSample(0, i));
            t += k;
        }
        return out;
    }
};

static double band(const std::vector<float>& x, size_t a, size_t b, double f) {
    double re = 0, im = 0;
    for (size_t i = a; i < b; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2 * PI * (double) (i - a) / (double) (b - a));
        re += x[i] * w * std::cos(2 * PI * f * (double) i / sr);
        im += x[i] * w * std::sin(2 * PI * f * (double) i / sr);
    }
    return 4.0 * std::sqrt(re * re + im * im) / (double) (b - a);
}
static double freqOf(const std::vector<float>& x, size_t a, size_t b) {
    double first = -1, last = -1;
    int cnt = 0;
    for (size_t i = a + 1; i < b; ++i)
        if (x[i - 1] < 0 && x[i] >= 0) {
            const double tt = (double) i - 1 + x[i - 1] / (x[i - 1] - x[i]);
            if (first < 0) first = tt;
            last = tt;
            ++cnt;
        }
    return cnt > 1 ? (cnt - 1) * sr / (last - first) : 0.0;
}
static Sig sine(double f, double a = 0.4) { return [=](long t) { return (float) (a * std::sin(2 * PI * f * t / sr)); }; }
static Sig saw(double f) {
    return [=](long t) {
        double s = 0;
        for (int h = 1; h <= 20; ++h) s += std::sin(2 * PI * h * f * t / sr) / h;
        return (float) (0.2 * s);
    };
}
static juce::MidiMessage on(int n) { return juce::MidiMessage::noteOn(1, n, 1.0f); }
static juce::MidiMessage off(int n) { return juce::MidiMessage::noteOff(1, n); }

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI gui;

    Rig r;
    check(r.p.getLatencySamples() == pvoc::SpectralFx::LATENCY, "latence annoncee a l'hote : " + juce::String(r.p.getLatencySamples()));
    check(r.p.getBusCount(true) == 2 && r.p.getBus(true, 1)->isEnabled(), "bus sidechain disponible");
    {   // neutre : signal retarde de la latence, sans alteration
        auto o = r.run(96000, sine(440));
        double e = 0, s = 0;
        for (size_t i = 20000; i < o.size(); ++i) {
            const double ref = 0.4 * std::sin(2 * PI * 440 * (double) (i - (size_t) pvoc::SpectralFx::LATENCY) / sr);
            e += (o[i] - ref) * (o[i] - ref);
            s += ref * ref;
        }
        check(10 * std::log10(e / s) < -60, "neutre : son intact (ecart " + juce::String(10 * std::log10(e / s), 1) + " dB)");
    }
    {   // pitch
        r.set("pitch", 7.0f);
        auto o = r.run(96000, sine(440));
        check(std::abs(freqOf(o, 24000, o.size()) - 659.26) < 0.3, "parametre Hauteur +7 : " + juce::String(freqOf(o, 24000, o.size()), 2) + " Hz");
        r.set("pitch", 0.0f);
        r.set("fshift", 250.0f);
        o = r.run(96000, sine(440));
        check(std::abs(freqOf(o, 24000, o.size()) - 690) < 0.5, "decalage de frequence +250 Hz : " + juce::String(freqOf(o, 24000, o.size()), 2) + " Hz");
        r.set("fshift", 0.0f);
        r.run(24000, sine(440));
    }
    {   // harmoniseur au clavier (reference C3 = 60)
        auto o = r.run(96000, sine(440), nullptr, { on(64), on(67) });
        const double a0 = band(o, 24000, o.size(), 440), a4 = band(o, 24000, o.size(), 554.37), a7 = band(o, 24000, o.size(), 659.26);
        check(a0 > 0.35 && a4 > 0.35 && a7 > 0.35, "clavier E3 + G3 : tierce et quinte ajoutees (" + juce::String(a4, 3) + ", " + juce::String(a7, 3) + ")");
        check(r.p.getActiveVoices() == 2, "2 voix actives");
        r.set("keepMain", 0.0f);
        o = r.run(48000, sine(440));
        check(band(o, 12000, o.size(), 440) < 0.01, "sans le son principal pendant les notes");
        o = r.run(96000, sine(440), nullptr, { off(64), off(67) });
        check(band(o, 48000, o.size(), 440) > 0.35 && band(o, 48000, o.size(), 554.37) < 0.01, "notes relachees : le son principal revient");
        r.set("keepMain", 1.0f);
        // pitch bend +2 sur une note
        o = r.run(96000, sine(440), nullptr, { juce::MidiMessage::pitchWheel(1, 16383), on(60) });
        r.set("keepMain", 0.0f);
        o = r.run(96000, sine(440));
        check(std::abs(freqOf(o, 24000, o.size()) - 440 * std::pow(2.0, 2.0 * 8191 / 8192 / 12)) < 0.6,
              "pitch bend +2 : " + juce::String(freqOf(o, 24000, o.size()), 2) + " Hz");
        r.run(48000, sine(440), nullptr, { off(60), juce::MidiMessage::pitchWheel(1, 8192) });
        r.set("keepMain", 1.0f);
    }
    {   // pedale de sustain = gel
        const long t0 = r.t, switchAt = t0 + 200000;   // l'entree passe de 440 a 660 Hz pendant le gel
        Sig in = [switchAt](long t) { return (float) (0.4 * std::sin(2 * PI * (t < switchAt ? 440 : 660) * t / sr)); };
        r.run(24000, in);
        r.run(24000, in, nullptr, { juce::MidiMessage::controllerEvent(1, 64, 127) });
        check(r.p.isFrozenNow(), "pedale enfoncee : gel");
        while (r.t < switchAt + 24000) r.run(24000, in);
        auto o = r.run(96000, in);
        check(std::abs(freqOf(o, 12000, o.size()) - 440) < 0.3, "gel : le son reste a 440 Hz alors que l'entree passe a 660 Hz");
        o = r.run(96000, in, nullptr, { juce::MidiMessage::controllerEvent(1, 64, 0) });
        check(!r.p.isFrozenNow() && std::abs(freqOf(o, 24000, o.size()) - 660) < 0.3, "pedale relachee : retour au direct (660 Hz)");
        // pedale en mode sustain des notes
        r.set("pedalFreeze", 0.0f);
        r.run(24000, in, nullptr, { juce::MidiMessage::controllerEvent(1, 64, 127), on(72), off(72) });
        o = r.run(48000, in);
        check(!r.p.isFrozenNow() && band(o, 12000, o.size(), 1320) > 0.3, "pedale = sustain : la note tient apres le relachement");
        r.run(48000, in, nullptr, { juce::MidiMessage::controllerEvent(1, 64, 0) });
        r.set("pedalFreeze", 1.0f);
        r.run(48000, in);
    }
    {   // robot joue au clavier (vocodeur) : A2 = 110 Hz
        r.set("kbMode", 1.0f);
        r.set("robot", 100.0f);
        juce::Random rnd(7);
        Sig noise = [&](long) { return (rnd.nextFloat() - 0.5f) * 0.6f; };
        auto o = r.run(96000, noise, nullptr, { on(45) });
        double harm = 0, between = 0;
        for (int h = 2; h <= 10; ++h) { harm += band(o, 24000, o.size(), 110.0 * h); between += band(o, 24000, o.size(), 110.0 * h + 55.0); }
        check(harm > 8 * between, "vocodeur au clavier : la note A1 (110 Hz) donne un peigne harmonique");
        check(r.p.getActiveVoices() == 1, "robot : une voix");
        r.run(48000, noise, nullptr, { off(45) });
        r.set("robot", 0.0f);
        r.set("kbMode", 0.0f);
    }
    {   // cross-synthese par le sidechain
        r.set("cross", 100.0f);
        r.set("crossSmooth", 25.0f);
        auto o = r.run(96000, saw(110), sine(1500, 0.5));
        const double nearB = band(o, 24000, o.size(), 1540), farB = band(o, 24000, o.size(), 220);
        check(nearB > 20 * farB, "cross-synthese par le sidechain : enveloppe imposee (" + juce::String(nearB / std::max(1e-9, farB), 0) + "x)");
        check(r.p.sidechainConnected(), "sidechain detecte");
        // fichier modulateur
        const auto f = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("pvoc_mod_1500.wav");
        {
            f.deleteFile();
            juce::WavAudioFormat fmt;
            std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream>(f);
            auto w = fmt.createWriterFor(os, juce::AudioFormatWriterOptions {}.withSampleRate(44100).withNumChannels(1).withBitsPerSample(24));
            juce::AudioBuffer<float> b(1, 44100);
            for (int i = 0; i < 44100; ++i) b.setSample(0, i, 0.5f * (float) std::sin(2 * PI * 1500 * i / 44100.0));
            w->writeFromAudioSampleBuffer(b, 0, 44100);
        }
        check(r.p.loadModulatorNow(f).isEmpty(), "fichier modulateur charge (44,1 kHz -> 48 kHz)");
        r.set("crossSource", 1.0f);
        o = r.run(96000, saw(110));
        check(band(o, 24000, o.size(), 1540) > 20 * band(o, 24000, o.size(), 220), "cross-synthese par fichier en boucle");
        // etat
        r.set("whisper", 12.0f);
        juce::MemoryBlock st;
        r.p.getStateInformation(st);
        Rig q;
        q.p.setStateInformation(st.getData(), (int) st.getSize());
        for (int i = 0; i < 300 && (q.p.isLoading() || q.p.getModulatorFile() == juce::File()); ++i) juce::Thread::sleep(10);
        check(q.p.getModulatorFile() == f && std::abs(q.p.params.getRawParameterValue("whisper")->load() - 12.0f) < 0.01f
                  && (int) q.p.params.getRawParameterValue("crossSource")->load() == 1,
              "etat : reglages et fichier modulateur restaures");
        r.set("whisper", 0.0f);
    }

    if (argc > 1) {
        r.set("cross", 35.0f);
        r.set("robot", 20.0f);
        r.set("pitch", 3.0f);
        r.set("fshift", -40.0f);
        r.set("blur", 30.0f);
        std::unique_ptr<juce::AudioProcessorEditor> ed(r.p.createEditor());
        ed->setVisible(true);
        for (int k = 0; k < 20; ++k) {
            r.run(4096, saw(110));
            juce::MessageManager::getInstance()->runDispatchLoopUntil(40);
        }
        r.p.keyboardState.noteOn(1, 64, 1.0f);
        r.p.keyboardState.noteOn(1, 67, 1.0f);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(200);
        auto img = ed->createComponentSnapshot(ed->getLocalBounds(), true, 2.0f);
        juce::File png(argv[1]);
        png.deleteFile();
        juce::FileOutputStream os(png);
        juce::PNGImageFormat().writeImageToStream(img, os);
        check(png.getSize() > 0, "interface : capture " + png.getFullPathName());
    }
    std::printf("\n%s (%d echec(s))\n", failures == 0 ? "TOUS LES TESTS DU PLUGIN EFFET PASSENT" : "DES TESTS ECHOUENT", failures);
    return failures == 0 ? 0 : 1;
}
