// Test du plugin complet (processeur JUCE + interface), sans hote.
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"

#include <cmath>
#include <cstdio>
#include <random>

static int failures = 0;
static void check(bool ok, const juce::String& what) {
    std::printf("%s %s\n", ok ? "[OK]  " : "[ECHEC]", what.toRawUTF8());
    if (!ok) ++failures;
}

static void setParam(PvocProcessor& p, const char* id, float v) {
    auto* prm = p.params.getParameter(id);
    prm->setValueNotifyingHost(prm->convertTo0to1(v));
}

/** Envoie n echantillons par blocs de tailles aleatoires ; renvoie le canal gauche. */
static std::vector<float> run(PvocProcessor& p, int n, std::function<float(int)> input = nullptr) {
    std::vector<float> out;
    std::mt19937 g(5);
    juce::MidiBuffer midi;
    int pos = 0;
    while (pos < n) {
        const int b = std::min(n - pos, 1 + (int) (g() % 1024));
        juce::AudioBuffer<float> buf(2, b);
        for (int i = 0; i < b; ++i) {
            const float x = input ? input(pos + i) : 0.0f;
            buf.setSample(0, i, x);
            buf.setSample(1, i, x);
        }
        p.processBlock(buf, midi);
        for (int i = 0; i < b; ++i) out.push_back(buf.getSample(0, i));
        pos += b;
    }
    return out;
}

static double freqOf(const std::vector<float>& x, size_t a, size_t b, double sr) {
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

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI gui;
    const double sr = 48000.0;

    // fichier de test : 2 s a 44,1 kHz, 1 kHz a gauche et a droite, 24 bits
    const juce::File wav = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("pvoc_test_44k.wav");
    {
        wav.deleteFile();
        juce::WavAudioFormat fmt;
        std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream>(wav);
        auto w = fmt.createWriterFor(os, juce::AudioFormatWriterOptions{}.withSampleRate(44100).withNumChannels(2).withBitsPerSample(24));
        juce::AudioBuffer<float> b(2, 88200);
        for (int i = 0; i < 88200; ++i) {
            const float v = 0.5f * (float) std::sin(2 * juce::MathConstants<double>::pi * 1000.0 * i / 44100.0);
            b.setSample(0, i, v);
            b.setSample(1, i, v);
        }
        w->writeFromAudioSampleBuffer(b, 0, 88200);
    }

    // ---------- 1) lecteur : rééchantillonnage 44,1 -> 48 kHz, hauteur, duree
    PvocProcessor p;
    p.setPlayConfigDetails(2, 2, sr, 512);
    p.prepareToPlay(sr, 512);
    check(p.loadFileNow(wav).isEmpty(), "chargement du fichier wav 24 bits a 44,1 kHz");
    check(std::abs(p.getDurationSeconds() - 2.0) < 0.001, "duree " + juce::String(p.getDurationSeconds(), 4) + " s (attendu 2 s)");
    check(p.getFileInfo().contains("48.0"), "info : " + p.getFileInfo());
    run(p, 512);                      // le son passe au thread audio
    p.requestPlay();
    auto out = run(p, (int) (sr * 2.6));
    const double f1 = freqOf(out, 12000, 80000, sr);
    check(std::abs(f1 - 1000.0) < 0.5, "lecteur : " + juce::String(f1, 2) + " Hz (attendu 1000)");
    check(std::abs(peak(out, 12000, 80000) - 0.5) < 0.01, "lecteur : niveau " + juce::String(peak(out, 12000, 80000), 3));
    check(!p.isPlayingNow(), "lecteur : arret automatique en fin de fichier");
    check(peak(out, (size_t) (sr * 2.3), out.size()) < 1e-3, "lecteur : silence apres la fin");

    // ---------- 2) transposition +12 et vitesse 50 %
    setParam(p, "transpose", 12.0f);
    setParam(p, "speed", 50.0f);
    p.requestStop();
    run(p, 4800);
    p.requestPlay();
    out = run(p, (int) (sr * 1.5));
    const double f2 = freqOf(out, 12000, 60000, sr);
    check(std::abs(f2 - 2000.0) < 1.0, "transposition +12 : " + juce::String(f2, 2) + " Hz (attendu 2000)");
    check(p.isPlayingNow() && std::abs(p.getPosition() - 0.375) < 0.02,
          "vitesse 50 % : position " + juce::String(p.getPosition(), 3) + " apres 1,5 s (attendu 0,375)");

    // ---------- 3) sauvegarde / restauration de l'etat
    juce::MemoryBlock state;
    p.getStateInformation(state);
    {
        PvocProcessor q;
        q.setPlayConfigDetails(2, 2, sr, 512);
        q.prepareToPlay(sr, 512);
        q.setStateInformation(state.getData(), (int) state.getSize());
        for (int i = 0; i < 400 && (q.isLoading() || q.getFileName().isEmpty()); ++i) juce::Thread::sleep(10);
        check(q.getFileName() == wav.getFileName(), "etat : fichier recharge (" + q.getFileName() + ")");
        check(std::abs(q.params.getRawParameterValue("transpose")->load() - 12.0f) < 1e-3, "etat : transposition restauree");
        check(std::abs(q.params.getRawParameterValue("speed")->load() - 50.0f) < 1e-3, "etat : vitesse restauree");
    }

    // ---------- 4) mode effet : latence et transposition en direct
    setParam(p, "mode", 1.0f);
    setParam(p, "transpose", 7.0f);
    setParam(p, "mix", 100.0f);
    p.prepareToPlay(sr, 512);
    check(p.getLatencySamples() == pvoc::Engine::EFFECT_LATENCY,
          "effet : latence annoncee " + juce::String(p.getLatencySamples()) + " ech");
    out = run(p, (int) (sr * 2), [&](int i) { return 0.4f * (float) std::sin(2 * juce::MathConstants<double>::pi * 440.0 * i / sr); });
    const double f3 = freqOf(out, (size_t) sr, out.size() - 1000, sr);
    check(std::abs(f3 - 440.0 * std::pow(2.0, 7.0 / 12.0)) < 0.5, "effet +7 : " + juce::String(f3, 2) + " Hz (attendu 659,26)");
    check(std::abs(peak(out, (size_t) sr, out.size() - 1000) - 0.4) < 0.01, "effet : niveau " + juce::String(peak(out, (size_t) sr, out.size() - 1000), 3));
    // regression : l'hote reapplique l'etat (son charge) pendant le mode effet -> la sortie doit rester active
    {
        juce::MemoryBlock st;
        p.getStateInformation(st);
        p.setStateInformation(st.getData(), (int) st.getSize());
        for (int i = 0; i < 200 && p.isLoading(); ++i) juce::Thread::sleep(10);
        out = run(p, (int) sr, [&](int i) { return 0.4f * (float) std::sin(2 * juce::MathConstants<double>::pi * 440.0 * i / sr); });
        check(std::abs(peak(out, (size_t) (sr / 2), out.size()) - 0.4) < 0.01,
              "effet apres restauration de l'etat : niveau " + juce::String(peak(out, (size_t) (sr / 2), out.size()), 3));
    }
    // regression : chargement d'un nouveau son pendant le mode effet
    {
        check(p.loadFileNow(wav).isEmpty(), "effet : chargement d'un son pendant le mode effet");
        out = run(p, (int) sr, [&](int i) { return 0.4f * (float) std::sin(2 * juce::MathConstants<double>::pi * 440.0 * i / sr); });
        check(std::abs(peak(out, (size_t) (sr / 2), out.size()) - 0.4) < 0.01,
              "effet apres chargement : niveau " + juce::String(peak(out, (size_t) (sr / 2), out.size()), 3));
    }
    setParam(p, "mode", 0.0f);
    p.prepareToPlay(sr, 512);
    check(p.getLatencySamples() == 0, "lecteur : latence 0");
    // retour au lecteur : la lecture doit refonctionner
    setParam(p, "transpose", 0.0f);
    setParam(p, "speed", 100.0f);
    p.requestStop();
    run(p, 4800);
    p.requestPlay();
    out = run(p, (int) sr);
    check(std::abs(freqOf(out, 12000, 40000, sr) - 1000.0) < 0.5 && peak(out, 12000, 40000) > 0.45,
          "retour au lecteur apres le mode effet : " + juce::String(freqOf(out, 12000, 40000, sr), 2) + " Hz");

    // ---------- 5) scrub depuis l'interface (lecteur en pause)
    {
        setParam(p, "scrub", 1.0f);
        setParam(p, "transpose", 0.0f);
        p.requestStop();
        run(p, 9600);
        auto silent = run(p, 9600);
        check(peak(silent, 0, silent.size()) < 1e-4, "scrub : silence a l'arret");
        p.beginScrub(0.2);
        run(p, 4800);
        p.moveScrub(0.7);
        out = run(p, (int) (sr / 2));
        check(std::abs(freqOf(out, 12000, out.size(), sr) - 1000.0) < 1.0 && peak(out, 12000, out.size()) > 0.45,
              "scrub : son audible en deplacant le curseur (" + juce::String(freqOf(out, 12000, out.size(), sr), 1) + " Hz)");
        check(std::abs(p.getPosition() - 0.7) < 0.01, "scrub : position " + juce::String(p.getPosition(), 3));
        p.endScrub(0.7);
        run(p, 4800);
        out = run(p, 9600);
        check(peak(out, 0, out.size()) < 1e-4 && !p.isPlayingNow(), "scrub : silence au relachement (pause)");
        setParam(p, "scrub", 0.0f);
    }

    // ---------- 6) position pilotee par le parametre (automation / MIDI)
    {
        setParam(p, "follow", 1.0f);
        setParam(p, "transpose", 0.0f);
        setParam(p, "speed", 100.0f);
        setParam(p, "position", 30.0f);
        p.requestStop();
        run(p, 9600);
        auto o = run(p, 9600);
        check(peak(o, 0, o.size()) < 1e-4, "pilotage : silence a l'arret");
        check(std::abs(p.getPosition() - 0.30) < 0.01, "pilotage : position suit le parametre a l'arret (" + juce::String(p.getPosition(), 3) + ")");
        p.requestPlay();
        o = run(p, (int) sr);
        check(std::abs(p.getPosition() - 0.30) < 0.01, "pilotage : en lecture, la position reste celle du parametre (" + juce::String(p.getPosition(), 3) + ")");
        check(std::abs(freqOf(o, 12000, o.size(), sr) - 1000.0) < 1.0 && peak(o, 12000, o.size()) > 0.45, "pilotage : son audible en lecture");
        // automation : rampe de 30 % a 80 % sur 1 s
        juce::MidiBuffer midi;
        for (int blk = 0; blk < 94; ++blk) {
            setParam(p, "position", 30.0f + 50.0f * (float) blk / 93.0f);
            juce::AudioBuffer<float> b(2, 512);
            b.clear();
            p.processBlock(b, midi);
        }
        run(p, 9600);
        check(std::abs(p.getPosition() - 0.80) < 0.01, "pilotage : automation suivie jusqu'a 80 % (" + juce::String(p.getPosition(), 3) + ")");
        // lissage reglable : 0 ms -> la position saute immediatement
        setParam(p, "smooth", 0.0f);
        setParam(p, "position", 20.0f);
        run(p, 2048);
        check(std::abs(p.getPosition() - 0.20) < 0.002, "lissage 0 ms : position immediate (" + juce::String(p.getPosition(), 3) + ")");
        setParam(p, "smooth", 1000.0f);
        setParam(p, "position", 90.0f);
        run(p, 24064);
        check(p.getPosition() > 0.40 && p.getPosition() < 0.56, "lissage 1000 ms : glissement lent (" + juce::String(p.getPosition(), 3) + ")");
        setParam(p, "smooth", 80.0f);
        // effet gele : le parametre parcourt l'historique
        setParam(p, "mode", 1.0f);
        p.prepareToPlay(sr, 512);
        run(p, (int) (sr * 3), [&](int i) { return 0.4f * (float) std::sin(2 * juce::MathConstants<double>::pi * 440.0 * i / sr); });
        setParam(p, "freeze", 1.0f);
        setParam(p, "position", 40.0f);
        run(p, (int) sr);
        check(std::abs(p.getPosition() - 0.40) < 0.01, "pilotage : effet gele, position dans l'historique (" + juce::String(p.getPosition(), 3) + ")");
        setParam(p, "freeze", 0.0f);
        setParam(p, "mode", 0.0f);
        setParam(p, "follow", 0.0f);
        p.prepareToPlay(sr, 512);
    }

    // ---------- 7) interface : creation et capture d'ecran
    if (argc > 1) {
        setParam(p, "transpose", 3.5f);
        setParam(p, "speed", 75.0f);
        setParam(p, "scrub", 1.0f);
        setParam(p, "follow", 1.0f);
        setParam(p, "position", 62.0f);
        std::unique_ptr<juce::AudioProcessorEditor> ed(p.createEditor());
        ed->setVisible(true);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(200);
        auto img = ed->createComponentSnapshot(ed->getLocalBounds(), true, 2.0f);
        juce::File png(argv[1]);
        png.deleteFile();
        juce::FileOutputStream os(png);
        juce::PNGImageFormat().writeImageToStream(img, os);
        check(png.getSize() > 0, "interface : capture " + png.getFullPathName());
    }

    std::printf("\n%s (%d echec(s))\n", failures == 0 ? "TOUS LES TESTS PASSENT" : "DES TESTS ECHOUENT", failures);
    return failures == 0 ? 0 : 1;
}
