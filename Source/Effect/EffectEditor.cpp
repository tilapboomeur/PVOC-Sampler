#include "EffectEditor.h"
#include "../Sampler/Zones.h"   // noms des notes

namespace {
const juce::Colour bg(0xff141b2d), header(0xff1f2a44), cardCol(0xff1f2a44), accent(0xfff2b84b),
    text(0xffe8ecf6), textDim(0xffb7c0d8), onAccent(0xff1f2a44), panel(0xff2a3654);
constexpr int W = 900, Hh = 812;
const juce::String audioExt = "wav;aif;aiff;flac;ogg;mp3;m4a;caf;aac";
} // namespace

// ------------------------------------------------------------------ spectre

void SpectrumView::update() {
    for (int b = 0; b < pvoc::SpectralFx::SPEC_BANDS; ++b) {
        const float i = fx.specIn[b].load(), o = fx.specOut[b].load();
        inDb[b] = i > inDb[b] ? i : inDb[b] * 0.8f + i * 0.2f;    // retombee douce
        outDb[b] = o > outDb[b] ? o : outDb[b] * 0.8f + o * 0.2f;
    }
    repaint();
}

void SpectrumView::paint(juce::Graphics& g) {
    const auto r = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff0f1524));
    g.fillRoundedRectangle(r, 8.0f);
    const auto plot = r.reduced(10.0f, 8.0f).withTrimmedBottom(12.0f);
    // reperes de frequence
    g.setFont(juce::FontOptions(10.0f));
    for (double f : { 100.0, 1000.0, 10000.0 }) {
        const float x = plot.getX() + plot.getWidth() * (float) (std::log(f / 30.0) / std::log(20000.0 / 30.0));
        g.setColour(juce::Colour(0xff35415f));
        g.drawVerticalLine((int) x, plot.getY(), plot.getBottom());
        g.setColour(textDim.withAlpha(0.7f));
        g.drawText(f >= 1000 ? juce::String((int) (f / 1000)) + " kHz" : juce::String((int) f) + " Hz",
                   juce::Rectangle<float>(x - 30.0f, plot.getBottom() + 1.0f, 60.0f, 12.0f), juce::Justification::centred);
    }
    auto path = [&](const float* db) {
        juce::Path p;
        const int nb = pvoc::SpectralFx::SPEC_BANDS;
        for (int b = 0; b < nb; ++b) {
            const float x = plot.getX() + plot.getWidth() * ((float) b + 0.5f) / (float) nb;
            const float v = juce::jlimit(0.0f, 1.0f, (db[b] + 90.0f) / 90.0f);
            const float y = plot.getBottom() - v * plot.getHeight();
            if (b == 0) p.startNewSubPath(x, y);
            else p.lineTo(x, y);
        }
        return p;
    };
    g.setColour(textDim.withAlpha(0.55f));
    g.strokePath(path(inDb), juce::PathStrokeType(1.5f));
    g.setColour(accent);
    g.strokePath(path(outDb), juce::PathStrokeType(2.0f));
    g.setFont(juce::FontOptions(11.0f));
    g.setColour(textDim);
    g.drawText(fxs("entrée"), plot.withTrimmedRight(70.0f).withHeight(14.0f), juce::Justification::topRight);
    g.setColour(accent);
    g.drawText("sortie", plot.withHeight(14.0f), juce::Justification::topRight);
}

// ------------------------------------------------------------------ editeur

EffectEditor::EffectEditor(EffectProcessor& p)
    : AudioProcessorEditor(p), proc(p), spectrum(p.engine()),
      keyboard(p.keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard) {
    lnf.setColourScheme({ bg, header, cardCol, textDim, text, accent, onAccent, header, text });
    lnf.setColour(juce::Slider::thumbColourId, accent);
    lnf.setColour(juce::Slider::trackColourId, accent.withAlpha(0.8f));
    lnf.setColour(juce::Slider::backgroundColourId, juce::Colour(0xff35415f));
    lnf.setColour(juce::ToggleButton::tickColourId, accent);
    lnf.setColour(juce::ComboBox::backgroundColourId, panel);
    lnf.setColour(juce::TextButton::buttonColourId, panel);
    lnf.setColour(juce::TextButton::buttonOnColourId, accent);
    lnf.setColour(juce::TextButton::textColourOnId, onAccent);
    setLookAndFeel(&lnf);
    auto& ps = proc.params;

    // ----- gel
    freezeButton.setButtonText("GEL");
    freezeButton.setClickingTogglesState(true);
    freezeButton.setTooltip(fxs("Fige le son présent à cet instant (la pédale de sustain peut aussi le déclencher)."));
    addAndMakeVisible(freezeButton);
    bAtt.push_back(std::make_unique<BA>(ps, "freeze", freezeButton));
    pedalT.setButtonText(fxs("Pédale de sustain = gel"));
    addAndMakeVisible(pedalT);
    bAtt.push_back(std::make_unique<BA>(ps, "pedalFreeze", pedalT));

    auto setup = [&](Ctl& c, const char* id, const juce::String& name, const juce::String& suffix, int decimals, double reset) {
        c.s.setSliderStyle(juce::Slider::LinearHorizontal);
        c.s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 86, 22);
        c.l.setText(name, juce::dontSendNotification);
        c.l.setColour(juce::Label::textColourId, text);
        addAndMakeVisible(c.s);
        addAndMakeVisible(c.l);
        sAtt.push_back(std::make_unique<SA>(ps, id, c.s));
        c.s.setTextValueSuffix(suffix);
        c.s.setNumDecimalPlacesToDisplay(decimals);
        c.s.setDoubleClickReturnValue(true, reset);
    };
    setup(pitch, "pitch", "Hauteur", " dt", 2, 0.0);
    setup(shift, "fshift", fxs("Décalage de fréquence"), " Hz", 1, 0.0);
    setup(robot, "robot", "Robotisation", " %", 0, 0.0);
    setup(robotHz, "robotHz", "Hauteur du robot", " Hz", 1, 110.0);
    setup(whisper, "whisper", "Chuchotement", " %", 0, 0.0);
    setup(blur, "blur", "Flou spectral", " %", 0, 0.0);
    setup(gate, "gate", "Porte spectrale", " dB", 1, -90.0);
    gate.s.setTextValueSuffix({});
    gate.s.textFromValueFunction = [](double v) { return v <= -89.95 ? fxs("inactive") : juce::String(v, 1) + " dB"; };
    gate.s.updateText();
    setup(cross, "cross", fxs("Quantité de cross-synthèse"), " %", 0, 0.0);
    setup(crossSmooth, "crossSmooth", fxs("Lissage de l'enveloppe"), " %", 0, 40.0);
    setup(attack, "attack", "Attaque des voix", " ms", 0, 10.0);
    setup(release, "release", fxs("Relâchement des voix"), " ms", 0, 200.0);
    setup(mix, "mix", fxs("Mélange sec / traité"), " %", 0, 100.0);
    setup(gain, "gain", "Volume", " dB", 1, 0.0);
    setup(bend, "bend", "Plage du pitch bend", " dt", 0, 2.0);
    setup(refNote, "refNote", fxs("Note de référence (son d'origine)"), "", 0, 60.0);
    refNote.s.setSliderStyle(juce::Slider::IncDecButtons);
    refNote.s.setIncDecButtonsMode(juce::Slider::incDecButtonsDraggable_Vertical);
    refNote.s.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 110, 24);
    refNote.s.textFromValueFunction = [](double v) { return pvoc::noteName((int) v) + " (" + pvoc::noteNameFr((int) v) + ")"; };
    refNote.s.updateText();

    auto quick = [&](juce::TextButton& b, const juce::String& t, Ctl& c, double v) {
        b.setButtonText(t);
        b.onClick = [&c, v] { c.s.setValue(v, juce::sendNotificationSync); };
        addAndMakeVisible(b);
    };
    quick(pm12, fxs("−12"), pitch, -12.0);
    quick(p0, "0", pitch, 0.0);
    quick(pp12, "+12", pitch, 12.0);
    quick(shift0, "0 Hz", shift, 0.0);

    // ----- cross-synthese
    crossSource.addItemList({ fxs("Sidechain (piste du DAW)"), fxs("Fichier audio en boucle") }, 1);
    addAndMakeVisible(crossSource);
    cAtt.push_back(std::make_unique<CA>(ps, "crossSource", crossSource));
    crossSourceL.setText("Modulateur", juce::dontSendNotification);
    crossSourceL.setColour(juce::Label::textColourId, text);
    addAndMakeVisible(crossSourceL);
    loadMod.setButtonText(fxs("Charger un son…"));
    loadMod.onClick = [this] {
        chooser = std::make_unique<juce::FileChooser>(fxs("Son modulateur"), juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3;*.m4a;*.caf;*.aac");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) {
            const auto f = fc.getResult();
            if (f.existsAsFile()) {
                proc.loadModulator(f);
                if (auto* prm = proc.params.getParameter("crossSource")) prm->setValueNotifyingHost(1.0f);
            }
        });
    };
    clearMod.setButtonText(fxs("Retirer"));
    clearMod.onClick = [this] { proc.clearModulator(); };
    modName.setColour(juce::Label::textColourId, text);
    modStatus.setColour(juce::Label::textColourId, textDim);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &loadMod, &clearMod, &modName, &modStatus }) addAndMakeVisible(*c);

    // ----- clavier
    kbMode.addItemList({ fxs("Harmoniseur : chaque note ajoute une voix transposée"), fxs("Robot : les notes jouent le robot (vocodeur)"),
                         fxs("Les deux") }, 1);
    addAndMakeVisible(kbMode);
    cAtt.push_back(std::make_unique<CA>(ps, "kbMode", kbMode));
    kbModeL.setText("Clavier", juce::dontSendNotification);
    kbModeL.setColour(juce::Label::textColourId, text);
    addAndMakeVisible(kbModeL);
    keepMainT.setButtonText(fxs("Garder le son principal pendant les notes"));
    addAndMakeVisible(keepMainT);
    bAtt.push_back(std::make_unique<BA>(ps, "keepMain", keepMainT));

    addAndMakeVisible(spectrum);
    keyboard.setAvailableRange(0, 127);
    keyboard.setLowestVisibleKey(36);
    keyboard.setKeyWidth(20.0f);
    keyboard.setColour(juce::MidiKeyboardComponent::keyDownOverlayColourId, accent);
    keyboard.setColour(juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, accent.withAlpha(0.3f));
    addAndMakeVisible(keyboard);

    proc.addChangeListener(this);
    setSize(W, Hh);
    refresh();
    startTimerHz(30);
}

EffectEditor::~EffectEditor() {
    proc.removeChangeListener(this);
    setLookAndFeel(nullptr);
}

void EffectEditor::card(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& title) {
    g.setColour(cardCol);
    g.fillRoundedRectangle(r.toFloat(), 12.0f);
    g.setColour(accent);
    g.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    g.drawText(title, r.reduced(14, 8).withHeight(20), juce::Justification::centredLeft);
}

void EffectEditor::paint(juce::Graphics& g) {
    g.fillAll(bg);
    g.setColour(header);
    g.fillRect(0, 0, getWidth(), 72);
    g.setColour(text);
    g.setFont(juce::FontOptions(22.0f, juce::Font::bold));
    g.drawText("Effet PVoc", 16, 10, 300, 28, juce::Justification::centredLeft);
    g.setColour(textDim);
    g.setFont(juce::FontOptions(14.0f));
    g.drawText(fxs("Effets spectraux en direct · vocodeur de phase · jouable au clavier"), 16, 38, 460, 20,
               juce::Justification::centredLeft);
    card(g, rPitch, "Hauteur");
    card(g, rSpectral, "Effets spectraux");
    card(g, rCross, fxs("Cross-synthèse"));
    card(g, rKeys, "Clavier MIDI");
    card(g, rOut, "Sortie");
}

void EffectEditor::resized() {
    const int w = getWidth();
    freezeButton.setBounds(w - 176, 14, 160, 44);
    pedalT.setBounds(w - 400, 24, 216, 26);

    const int top = 84, cw = (w - 48) / 3, ch = 316;
    rPitch = { 16, top, cw, ch };
    rSpectral = { 24 + cw, top, cw, ch };
    rCross = { 32 + 2 * cw, top, cw, ch };
    auto rowIn = [](juce::Rectangle<int> r, int& y, Ctl& c) {
        c.l.setBounds(r.getX() + 14, y, r.getWidth() - 28, 20);
        c.s.setBounds(r.getX() + 8, y + 20, r.getWidth() - 20, 24);
        y += 50;
    };
    int y = top + 36;
    rowIn(rPitch, y, pitch);
    pm12.setBounds(rPitch.getX() + 14, y - 4, 56, 24);
    p0.setBounds(rPitch.getX() + 76, y - 4, 56, 24);
    pp12.setBounds(rPitch.getX() + 138, y - 4, 56, 24);
    y += 28;
    rowIn(rPitch, y, shift);
    shift0.setBounds(rPitch.getX() + 14, y - 4, 70, 24);

    y = top + 36;
    for (auto* c : { &robot, &robotHz, &whisper, &blur, &gate }) { rowIn(rSpectral, y, *c); y += 4; }

    y = top + 36;
    rowIn(rCross, y, cross);
    rowIn(rCross, y, crossSmooth);
    crossSourceL.setBounds(rCross.getX() + 14, y, rCross.getWidth() - 28, 20);
    crossSource.setBounds(rCross.getX() + 14, y + 22, rCross.getWidth() - 28, 26);
    y += 56;
    loadMod.setBounds(rCross.getX() + 14, y, 150, 28);
    clearMod.setBounds(rCross.getX() + 170, y, 80, 28);
    y += 32;
    modName.setBounds(rCross.getX() + 14, y, rCross.getWidth() - 28, 20);
    modStatus.setBounds(rCross.getX() + 14, y + 20, rCross.getWidth() - 28, 34);

    const int top2 = top + ch + 8, h2 = 196;
    rKeys = { 16, top2, 2 * cw + 8, h2 };
    rOut = { 32 + 2 * cw, top2, cw, h2 };
    kbModeL.setBounds(rKeys.getX() + 14, top2 + 34, 70, 26);
    kbMode.setBounds(rKeys.getX() + 84, top2 + 34, rKeys.getWidth() - 98, 26);
    const int half = (rKeys.getWidth() - 28) / 2;
    auto cell = [&](Ctl& c, int col, int yy) {
        c.l.setBounds(rKeys.getX() + 14 + col * half, yy, half - 10, 20);
        c.s.setBounds(rKeys.getX() + 8 + col * half, yy + 20, half - 12, 24);
    };
    cell(refNote, 0, top2 + 66);
    cell(bend, 1, top2 + 66);
    cell(attack, 0, top2 + 114);
    cell(release, 1, top2 + 114);
    keepMainT.setBounds(rKeys.getX() + 10, top2 + 164, rKeys.getWidth() - 24, 24);
    y = top2 + 36;
    rowIn(rOut, y, mix);
    rowIn(rOut, y, gain);

    spectrum.setBounds(16, top2 + h2 + 8, w - 32, 100);
    keyboard.setBounds(16, Hh - 92, w - 32, 82);
}

void EffectEditor::refresh() {
    const auto f = proc.getModulatorFile();
    modName.setText(f == juce::File() ? fxs("Aucun fichier chargé") : f.getFileName(), juce::dontSendNotification);
    clearMod.setEnabled(f != juce::File());
    if (proc.isLoading()) modStatus.setText(fxs("Chargement…"), juce::dontSendNotification);
}

void EffectEditor::timerCallback() {
    spectrum.update();
    juce::String st;
    const bool fileSrc = proc.params.getRawParameterValue("crossSource")->load() > 0.5f;
    const float lvl = proc.engine().modLevel.load();
    if (proc.getLastError().isNotEmpty()) st = proc.getLastError();
    else if (proc.isLoading()) st = fxs("Chargement…");
    else if (proc.params.getRawParameterValue("cross")->load() <= 0.0f) st = fxs("Montez la quantité pour entendre la cross-synthèse.");
    else if (!fileSrc && !proc.sidechainConnected()) st = fxs("Aucun sidechain : choisissez une piste dans le menu sidechain du DAW.");
    else if (fileSrc && proc.getModulatorFile() == juce::File()) st = fxs("Chargez un son modulateur.");
    else st = fxs("Niveau du modulateur : ") + juce::String(juce::roundToInt(juce::Decibels::gainToDecibels(lvl, -90.0f))) + " dB";
    modStatus.setText(st, juce::dontSendNotification);
    freezeButton.setToggleState(proc.isFrozenNow(), juce::dontSendNotification);
    const bool robotOn = proc.params.getRawParameterValue("robot")->load() > 0.0f;
    robotHz.s.setEnabled(robotOn);
    robotHz.l.setEnabled(robotOn);
}

bool EffectEditor::isInterestedInFileDrag(const juce::StringArray& files) {
    for (auto& f : files) if (juce::File(f).hasFileExtension(audioExt)) return true;
    return false;
}

void EffectEditor::filesDropped(const juce::StringArray& files, int, int) {
    for (auto& f : files) {
        const juce::File file(f);
        if (file.hasFileExtension(audioExt)) {
            proc.loadModulator(file);
            if (auto* prm = proc.params.getParameter("crossSource")) prm->setValueNotifyingHost(1.0f);
            break;
        }
    }
}
