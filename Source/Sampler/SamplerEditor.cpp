#include "SamplerEditor.h"

namespace {
const juce::Colour bg(0xff141b2d), header(0xff1f2a44), card(0xff1f2a44), accent(0xfff2b84b),
    text(0xffe8ecf6), textDim(0xffb7c0d8), onAccent(0xff1f2a44), zoneCol(0xff5b6b93);
constexpr int W = 840, Hh = 840;
constexpr int ZoneTop = 112, ZoneH = 440, GlobTop = ZoneTop + ZoneH + 12;
const juce::String audioExt = "wav;aif;aiff;flac;ogg;mp3;m4a;caf;aac";

juce::String noteLabel(int n) { return pvoc::noteName(n) + " (" + pvoc::noteNameFr(n) + ")"; }
} // namespace

SamplerEditor::SamplerEditor(SamplerProcessor& p)
    : AudioProcessorEditor(p), proc(p),
      keyboard(p.keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard) {
    lnf.setColourScheme({ bg, header, card, textDim, text, accent, onAccent, header, text });
    lnf.setColour(juce::Slider::thumbColourId, accent);
    lnf.setColour(juce::Slider::trackColourId, accent.withAlpha(0.8f));
    lnf.setColour(juce::Slider::backgroundColourId, juce::Colour(0xff35415f));
    lnf.setColour(juce::ToggleButton::tickColourId, accent);
    lnf.setColour(juce::ListBox::backgroundColourId, card);
    lnf.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff2a3654));
    lnf.setColour(juce::TextButton::buttonOnColourId, accent);
    lnf.setColour(juce::TextButton::textColourOnId, onAccent);
    setLookAndFeel(&lnf);

    // ----- en-tete
    addButton.setButtonText(frs("Ajouter des sons…"));
    addButton.setColour(juce::TextButton::buttonColourId, accent);
    addButton.setColour(juce::TextButton::textColourOffId, onAccent);
    addButton.onClick = [this] { openChooser(); };
    spreadButton.setButtonText(frs("Répartir sur le clavier"));
    spreadButton.setTooltip(frs("Recalcule les plages de toutes les zones à partir de leurs notes de base."));
    spreadButton.onClick = [this] { proc.autoSpread(); };
    clearButton.setButtonText("Tout retirer");
    clearButton.onClick = [this] { proc.removeAllZones(); };
    for (auto* b : { &addButton, &spreadButton, &clearButton }) addAndMakeVisible(*b);
    statusLabel.setColour(juce::Label::textColourId, textDim);
    statusLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(statusLabel);

    // ----- liste des zones
    list.setRowHeight(42);
    list.setColour(juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
    addAndMakeVisible(list);
    emptyHint.setText(frs("Glissez des sons ici\nou cliquez sur « Ajouter des sons… ».\n\n"
                          "Une note dans le nom du fichier\n(ex. Piano_C3.wav, Voix_Do3.wav)\n"
                          "devient sa note de base."),
                      juce::dontSendNotification);
    emptyHint.setJustificationType(juce::Justification::centred);
    emptyHint.setColour(juce::Label::textColourId, textDim);
    emptyHint.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(emptyHint);

    // ----- edition de la zone
    zoneTitle.setFont(juce::FontOptions(18.0f, juce::Font::bold));
    zoneTitle.setColour(juce::Label::textColourId, text);
    zoneInfo.setColour(juce::Label::textColourId, textDim);
    removeButton.setButtonText("Retirer ce son");
    removeButton.onClick = [this] { if (selectedId != 0) proc.removeZone(selectedId); };
    for (juce::Component* c : std::initializer_list<juce::Component*> { &zoneTitle, &zoneInfo, &removeButton })
        addAndMakeVisible(*c);

    auto setupNote = [this](juce::Slider& s, juce::Label& l, const juce::String& name) {
        s.setSliderStyle(juce::Slider::IncDecButtons);
        s.setIncDecButtonsMode(juce::Slider::incDecButtonsDraggable_Vertical);
        s.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 110, 24);
        s.setRange(0, 127, 1);
        s.textFromValueFunction = [](double v) { return noteLabel((int) v); };
        s.valueFromTextFunction = [](const juce::String& t) {
            const int n = pvoc::noteFromFileName("x_" + t.trim().replace(" ", ""));
            return n >= 0 ? (double) n : t.getDoubleValue();
        };
        s.onValueChange = [this] { pushZoneEdit(); };
        l.setText(name, juce::dontSendNotification);
        l.setColour(juce::Label::textColourId, text);
        addAndMakeVisible(s);
        addAndMakeVisible(l);
    };
    setupNote(root, rootL, "Note de base");
    setupNote(low, lowL, "Note la plus basse");
    setupNote(high, highL, "Note la plus haute");

    auto setupLin = [this](juce::Slider& s, juce::Label& l, const juce::String& name, double lo, double hi,
                           double step, const juce::String& suffix, int decimals) {
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 86, 22);
        s.setRange(lo, hi, step);
        s.setTextValueSuffix(suffix);
        s.setNumDecimalPlacesToDisplay(decimals);
        s.onValueChange = [this] { pushZoneEdit(); };
        l.setText(name, juce::dontSendNotification);
        l.setColour(juce::Label::textColourId, text);
        addAndMakeVisible(s);
        addAndMakeVisible(l);
    };
    setupLin(tune, tuneL, "Accord fin", -100, 100, 1, " cents", 0);
    setupLin(release, releaseL, frs("Relâchement"), 0, 5000, 1, " ms", 0);
    release.setSkewFactorFromMidPoint(400.0);
    setupLin(zgain, zgainL, "Volume du son", -40, 12, 0.1, " dB", 1);
    setupLin(zspeed, zspeedL, "Vitesse du son", 0, 400, 0.1, " %", 0);
    setupLin(zstart, zstartL, frs("Point de départ"), 0, 100, 0.1, " %", 1);

    mode.addItem(frs("Tenu : s'arrête au relâchement"), 1);
    mode.addItem(frs("One-shot : joue jusqu'à la fin"), 2);
    mode.onChange = [this] { pushZoneEdit(); };
    modeL.setText("Jeu", juce::dontSendNotification);
    modeL.setColour(juce::Label::textColourId, text);
    loop.setButtonText("Boucle");
    loop.onClick = [this] { pushZoneEdit(); };
    for (juce::Component* c : std::initializer_list<juce::Component*> { &mode, &modeL, &loop }) addAndMakeVisible(*c);
    releaseL.setText(frs("Relâchement du volume"), juce::dontSendNotification);

    // ----- onglets
    tabSound.setButtonText("Son");
    tabEnv.setButtonText("Enveloppe de position");
    for (auto* t : { &tabSound, &tabEnv }) {
        t->setClickingTogglesState(false);
        t->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2a3654));
        addAndMakeVisible(*t);
    }
    tabSound.setConnectedEdges(juce::Button::ConnectedOnRight);
    tabEnv.setConnectedEdges(juce::Button::ConnectedOnLeft);
    tabSound.onClick = [this] { setTab(0); };
    tabEnv.onClick = [this] { setTab(1); };

    // ----- enveloppe de position
    envOn.setButtonText(frs("Activer l'enveloppe de position (la position suit A, D, S, R)"));
    envOn.onClick = [this] { pushZoneEdit(); };
    addAndMakeVisible(envOn);
    addAndMakeVisible(envGraph);
    auto pos = [&](juce::Slider& s, juce::Label& l, const juce::String& name) {
        setupLin(s, l, name, 0, 100, 0.1, " %", 1);
    };
    pos(pStartS, pStartL, frs("Départ (début de la note)"));
    pos(pPeakS, pPeakL, frs("Pic (fin de l'attaque)"));
    pos(pSustainS, pSustainL, "Maintien (S)");
    pos(pEndS, pEndL, frs("Fin (après le relâchement)"));
    auto dur = [&](juce::Slider& s, juce::Label& l, const juce::String& name) {
        setupLin(s, l, name, 0, 10000, 1, " ms", 0);
        s.setSkewFactorFromMidPoint(600.0);
    };
    dur(aS, aL, frs("A : durée de l'attaque"));
    dur(dS, dL, frs("D : durée du déclin"));
    dur(rS, rL, frs("R : durée vers la fin"));
    lfoOn.setButtonText("LFO sur la position");
    lfoOn.onClick = [this] { pushZoneEdit(); };
    lfoShape.addItem("Sinus", 1);
    lfoShape.addItem("Triangle", 2);
    lfoShape.addItem(frs("Aléatoire doux"), 3);
    lfoShape.onChange = [this] { pushZoneEdit(); };
    addAndMakeVisible(lfoOn);
    addAndMakeVisible(lfoShape);
    setupLin(lfoRateS, lfoRateL, "Vitesse du LFO", 0.01, 20.0, 0.01, " Hz", 2);
    lfoRateS.setSkewFactorFromMidPoint(1.0);
    setupLin(lfoDepthS, lfoDepthL, "Profondeur du LFO", 0, 50, 0.1, " %", 1);
    lfoDepthS.setSkewFactorFromMidPoint(8.0);

    // ----- reglages globaux
    auto& ps = proc.params;
    auto setupGlobal = [&](juce::Slider& s, juce::Label& l, const char* id, const juce::String& name,
                           const juce::String& suffix, int decimals) {
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 86, 22);
        l.setText(name, juce::dontSendNotification);
        l.setColour(juce::Label::textColourId, text);
        addAndMakeVisible(s);
        addAndMakeVisible(l);
        sliderAtt.push_back(std::make_unique<SA>(ps, id, s));
        s.setTextValueSuffix(suffix);
        s.setNumDecimalPlacesToDisplay(decimals);
    };
    setupGlobal(voicesS, voicesL, "voices", "Voix (polyphonie)", "", 0);
    setupGlobal(speedS, speedL, "speed", "Vitesse (time-stretch)", " %", 0);
    setupGlobal(transposeS, transposeL, "transpose", "Hauteur globale", " dt", 2);
    setupGlobal(velocityS, velocityL, "velocity", frs("Sensibilité vélocité"), " %", 0);
    setupGlobal(bendS, bendL, "bend", "Plage du pitch bend", " dt", 0);
    setupGlobal(gainS, gainL, "gain", "Volume", " dB", 1);
    setupGlobal(positionS, positionL, "position", "Position", " %", 1);
    setupGlobal(smoothS, smoothL, "smooth", "Lissage", " ms", 0);
    voicesS.textFromValueFunction = [](double v) { return (int) v == 1 ? frs("mono") : juce::String((int) v); };
    followT.setButtonText(frs("Position pilotée (automation / MIDI)"));
    lockT.setButtonText("Verrouillage de phase");
    addAndMakeVisible(followT);
    addAndMakeVisible(lockT);
    buttonAtt.push_back(std::make_unique<BA>(ps, "follow", followT));
    buttonAtt.push_back(std::make_unique<BA>(ps, "lock", lockT));

    // ----- clavier
    keyboard.setAvailableRange(0, 127);
    keyboard.setLowestVisibleKey(24);
    keyboard.setKeyWidth(18.0f);
    keyboard.setColour(juce::MidiKeyboardComponent::keyDownOverlayColourId, accent);
    keyboard.setColour(juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, accent.withAlpha(0.3f));
    addAndMakeVisible(keyboard);

    proc.addChangeListener(this);
    setSize(W, Hh);
    reloadZones();
    startTimerHz(15);
}

SamplerEditor::~SamplerEditor() {
    proc.removeChangeListener(this);
    setLookAndFeel(nullptr);
}

// ------------------------------------------------------------------ dessin et mise en page

void SamplerEditor::paint(juce::Graphics& g) {
    g.fillAll(bg);
    g.setColour(header);
    g.fillRect(0, 0, getWidth(), 66);
    g.setColour(text);
    g.setFont(juce::FontOptions(22.0f, juce::Font::bold));
    g.drawText("Sampler PVoc", 16, 8, 300, 28, juce::Justification::centredLeft);
    g.setColour(textDim);
    g.setFont(juce::FontOptions(14.0f));
    g.drawText(frs("Sampler polyphonique à vocodeur de phase"), 16, 34, 360, 20, juce::Justification::centredLeft);

    g.setColour(card);
    g.fillRoundedRectangle(16.0f, (float) ZoneTop, 290.0f, (float) ZoneH, 12.0f);                          // liste
    g.fillRoundedRectangle(318.0f, (float) ZoneTop, (float) getWidth() - 334.0f, (float) ZoneH, 12.0f);    // zone
    g.fillRoundedRectangle(16.0f, (float) GlobTop, (float) getWidth() - 32.0f, 150.0f, 12.0f);             // globaux

    if (selectedId == 0) {
        g.setColour(textDim);
        g.setFont(juce::FontOptions(15.0f));
        g.drawText(frs("Sélectionnez un son dans la liste pour régler sa zone."),
                   juce::Rectangle<int>(318, ZoneTop, getWidth() - 334, ZoneH), juce::Justification::centred);
    }
}

void SamplerEditor::paintOverChildren(juce::Graphics& g) {
    // bande des zones au-dessus du clavier
    const auto kb = keyboard.getBounds();
    const int stripY = kb.getY() - 14;
    g.setColour(bg);
    g.fillRect(kb.getX(), stripY, kb.getWidth(), 12);
    auto keyX = [&](int n) {
        const auto r = keyboard.getRectangleForKey(n);
        return juce::Rectangle<float>(r.getX() + (float) kb.getX(), (float) stripY, r.getWidth(), 10.0f);
    };
    for (int pass = 0; pass < 2; ++pass)
        for (auto& z : zones) {
            const bool sel = z.id == selectedId;
            if ((pass == 0) == sel) continue;   // la zone selectionnee par-dessus
            auto a = keyX(z.settings.low), b = keyX(z.settings.high);
            auto r = a.getUnion(b).reduced(0.5f, sel ? 0.0f : 2.0f);
            r = r.getIntersection(kb.toFloat().withY((float) stripY).withHeight(10.0f));
            g.setColour(sel ? accent : zoneCol.withAlpha(0.8f));
            g.fillRoundedRectangle(r, 3.0f);
            const auto rk = keyX(z.settings.root);
            if (kb.toFloat().getHorizontalRange().contains(rk.getCentreX())) {
                g.setColour(sel ? onAccent : text);
                g.fillEllipse(rk.getCentreX() - 3.0f, (float) stripY + 2.0f, 6.0f, 6.0f);
            }
        }
    if (dragOver) {
        g.setColour(accent);
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(3.0f), 12.0f, 3.0f);
    }
}

void SamplerEditor::resized() {
    const int w = getWidth();
    addButton.setBounds(w - 236, 13, 220, 40);
    spreadButton.setBounds(w - 446, 19, 200, 28);
    statusLabel.setBounds(16, 76, w - 32, 26);
    clearButton.setBounds(16, 76, 110, 26);

    list.setBounds(24, ZoneTop + 8, 274, ZoneH - 16);
    emptyHint.setBounds(24, ZoneTop + 8, 274, ZoneH - 16);

    // zone selectionnee : titre, onglets, puis deux colonnes
    const int x0 = 334, colW = (w - 334 - 32) / 2;
    zoneTitle.setBounds(x0, ZoneTop + 6, w - x0 - 170, 26);
    zoneInfo.setBounds(x0, ZoneTop + 30, w - x0 - 170, 20);
    removeButton.setBounds(w - 160, ZoneTop + 10, 130, 28);
    tabSound.setBounds(x0, ZoneTop + 58, 120, 28);
    tabEnv.setBounds(x0 + 120, ZoneTop + 58, 200, 28);

    int y = ZoneTop + 98;
    auto row = [&](juce::Label& l, juce::Slider& s, int x) {
        l.setBounds(x, y, colW - 8, 20);
        s.setBounds(x, y + 20, colW - 16, 26);
    };
    // onglet Son
    row(rootL, root, x0);
    row(tuneL, tune, x0 + colW);
    y += 52;
    row(lowL, low, x0);
    row(highL, high, x0 + colW);
    y += 52;
    modeL.setBounds(x0, y, 60, 26);
    mode.setBounds(x0 + 40, y, colW - 56, 26);
    loop.setBounds(x0 + colW, y, 100, 26);
    y += 36;
    row(releaseL, release, x0);
    row(zgainL, zgain, x0 + colW);
    y += 52;
    row(zspeedL, zspeed, x0);
    row(zstartL, zstart, x0 + colW);

    // onglet Enveloppe de position
    y = ZoneTop + 94;
    envOn.setBounds(x0 - 4, y, w - x0 - 20, 24);
    y += 28;
    envGraph.setBounds(x0, y, w - x0 - 32, 70);
    y += 76;
    row(pStartL, pStartS, x0);
    row(aL, aS, x0 + colW);
    y += 46;
    row(pPeakL, pPeakS, x0);
    row(dL, dS, x0 + colW);
    y += 46;
    row(pSustainL, pSustainS, x0);
    row(rL, rS, x0 + colW);
    y += 46;
    row(pEndL, pEndS, x0);
    lfoOn.setBounds(x0 + colW - 4, y + 2, colW - 8, 22);
    lfoShape.setBounds(x0 + colW, y + 24, colW - 16, 24);
    y += 50;
    row(lfoRateL, lfoRateS, x0);
    row(lfoDepthL, lfoDepthS, x0 + colW);

    // reglages globaux : 3 colonnes
    const int gx = 30, gw = (w - 60) / 3;
    int gy = GlobTop + 8;
    auto grow = [&](juce::Label& l, juce::Slider& s, int col) {
        l.setBounds(gx + col * gw, gy, gw - 12, 20);
        s.setBounds(gx + col * gw, gy + 20, gw - 16, 24);
    };
    grow(voicesL, voicesS, 0);
    grow(speedL, speedS, 1);
    grow(transposeL, transposeS, 2);
    gy += 48;
    grow(velocityL, velocityS, 0);
    grow(bendL, bendS, 1);
    grow(gainL, gainS, 2);
    gy += 48;
    followT.setBounds(gx - 4, gy, gw - 4, 22);
    lockT.setBounds(gx - 4, gy + 22, gw - 4, 22);
    grow(positionL, positionS, 1);
    grow(smoothL, smoothS, 2);

    keyboard.setBounds(16, Hh - 100, w - 32, 88);
}

// ------------------------------------------------------------------ liste des zones

void SamplerEditor::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) {
    if (row < 0 || row >= (int) zones.size()) return;
    const auto& z = zones[(size_t) row];
    if (selected) {
        g.setColour(accent.withAlpha(0.18f));
        g.fillRoundedRectangle(2.0f, 2.0f, (float) w - 4.0f, (float) h - 4.0f, 6.0f);
        g.setColour(accent);
        g.fillRoundedRectangle(2.0f, 6.0f, 4.0f, (float) h - 12.0f, 2.0f);
    }
    g.setColour(text);
    g.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    g.drawText(z.file.getFileNameWithoutExtension(), 12, 3, w - 18, 20, juce::Justification::centredLeft, true);
    g.setColour(textDim);
    g.setFont(juce::FontOptions(13.0f));
    const auto& s = z.settings;
    g.drawText(pvoc::noteName(s.root) + frs("  ·  ") + pvoc::noteName(s.low) + frs(" → ") + pvoc::noteName(s.high)
                   + (s.mode == pvoc::PlayMode::OneShot ? frs("  ·  one-shot") : juce::String())
                   + (s.loop ? frs("  ·  boucle") : juce::String())
                   + (s.posEnv ? frs("  ·  enveloppe") : juce::String()),
               12, 22, w - 18, 18, juce::Justification::centredLeft, true);
}

void SamplerEditor::selectedRowsChanged(int lastRowSelected) {
    selectedId = (lastRowSelected >= 0 && lastRowSelected < (int) zones.size()) ? zones[(size_t) lastRowSelected].id : 0;
    showSelectedZone();
    repaint();
}

void SamplerEditor::deleteKeyPressed(int) {
    if (selectedId != 0) proc.removeZone(selectedId);
}

const SamplerProcessor::ZoneInfo* SamplerEditor::selectedZone() const {
    for (auto& z : zones) if (z.id == selectedId) return &z;
    return nullptr;
}

void SamplerEditor::reloadZones() {
    zones = proc.getZones();
    list.updateContent();
    // garder la selection, ou selectionner le dernier son ajoute
    int row = -1;
    for (int i = 0; i < (int) zones.size(); ++i) if (zones[(size_t) i].id == selectedId) row = i;
    if (row < 0 && !zones.empty()) row = (int) zones.size() - 1;
    if (row >= 0) list.selectRow(row, false, true);
    else { list.deselectAllRows(); selectedId = 0; }
    if (row >= 0) selectedId = zones[(size_t) row].id;
    emptyHint.setVisible(zones.empty());
    list.repaint();
    showSelectedZone();
    repaint();
}

void SamplerEditor::showSelectedZone() {
    const auto* z = selectedZone();
    const bool has = z != nullptr;
    for (juce::Component* c : std::initializer_list<juce::Component*> { &zoneTitle, &zoneInfo, &removeButton, &tabSound, &tabEnv })
        c->setVisible(has);
    for (juce::Component* c : std::initializer_list<juce::Component*> {
             &root, &low, &high, &tune, &release, &zgain, &zspeed, &zstart,
             &rootL, &lowL, &highL, &tuneL, &releaseL, &zgainL, &zspeedL, &zstartL, &mode, &modeL, &loop })
        c->setVisible(has && tab == 0);
    for (juce::Component* c : std::initializer_list<juce::Component*> {
             &envOn, &envGraph, &pStartS, &pPeakS, &pSustainS, &pEndS, &aS, &dS, &rS, &lfoRateS, &lfoDepthS,
             &pStartL, &pPeakL, &pSustainL, &pEndL, &aL, &dL, &rL, &lfoRateL, &lfoDepthL, &lfoOn, &lfoShape })
        c->setVisible(has && tab == 1);
    tabSound.setToggleState(tab == 0, juce::dontSendNotification);
    tabEnv.setToggleState(tab == 1, juce::dontSendNotification);
    if (!has) return;
    updatingUi = true;
    const auto& s = z->settings;
    zoneTitle.setText(z->file.getFileNameWithoutExtension(), juce::dontSendNotification);
    zoneInfo.setText(z->info + frs("  ·  ") + juce::String(z->durationSec, 2) + " s", juce::dontSendNotification);
    root.setValue(s.root, juce::dontSendNotification);
    low.setValue(s.low, juce::dontSendNotification);
    high.setValue(s.high, juce::dontSendNotification);
    tune.setValue(s.tuneCents, juce::dontSendNotification);
    release.setValue(s.releaseMs, juce::dontSendNotification);
    zgain.setValue(s.gainDb, juce::dontSendNotification);
    zspeed.setValue(s.speed, juce::dontSendNotification);
    zstart.setValue(s.start, juce::dontSendNotification);
    mode.setSelectedId(s.mode == pvoc::PlayMode::OneShot ? 2 : 1, juce::dontSendNotification);
    loop.setToggleState(s.loop, juce::dontSendNotification);
    release.setEnabled(s.mode == pvoc::PlayMode::Hold || s.posEnv);
    releaseL.setEnabled(s.mode == pvoc::PlayMode::Hold || s.posEnv);
    // avec l'enveloppe, c'est elle qui mene la position : vitesse et depart du son sont ignores
    for (juce::Component* c : std::initializer_list<juce::Component*> { &zspeed, &zspeedL, &zstart, &zstartL })
        c->setEnabled(!s.posEnv);
    envOn.setToggleState(s.posEnv, juce::dontSendNotification);
    pStartS.setValue(s.pStart, juce::dontSendNotification);
    pPeakS.setValue(s.pPeak, juce::dontSendNotification);
    pSustainS.setValue(s.pSustain, juce::dontSendNotification);
    pEndS.setValue(s.pEnd, juce::dontSendNotification);
    aS.setValue(s.aMs, juce::dontSendNotification);
    dS.setValue(s.dMs, juce::dontSendNotification);
    rS.setValue(s.rMs, juce::dontSendNotification);
    lfoOn.setToggleState(s.lfo, juce::dontSendNotification);
    lfoShape.setSelectedId(s.lfoShape + 1, juce::dontSendNotification);
    lfoRateS.setValue(s.lfoRate, juce::dontSendNotification);
    lfoDepthS.setValue(s.lfoDepth, juce::dontSendNotification);
    for (juce::Component* c : std::initializer_list<juce::Component*> {
             &pStartS, &pPeakS, &pSustainS, &pEndS, &aS, &dS, &rS, &pStartL, &pPeakL, &pSustainL, &pEndL, &aL, &dL, &rL, &lfoOn })
        c->setEnabled(s.posEnv);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &lfoShape, &lfoRateS, &lfoDepthS, &lfoRateL, &lfoDepthL })
        c->setEnabled(s.posEnv && s.lfo);
    // one-shot : pas de maintien
    pSustainL.setText(s.mode == pvoc::PlayMode::OneShot ? frs("Maintien (one-shot : point de passage)") : frs("Maintien (S)"),
                      juce::dontSendNotification);
    envGraph.setSettings(s);
    updatingUi = false;
}

void SamplerEditor::pushZoneEdit() {
    if (updatingUi) return;
    const auto* z = selectedZone();
    if (z == nullptr) return;
    auto s = z->settings;
    s.root = (int) root.getValue();
    s.low = (int) low.getValue();
    s.high = (int) high.getValue();
    if (s.high < s.low) {   // garder une plage valide
        if (&high == juce::Component::getCurrentlyFocusedComponent()) s.low = s.high;
        else s.high = s.low;
    }
    s.tuneCents = (float) tune.getValue();
    s.releaseMs = (float) release.getValue();
    s.gainDb = (float) zgain.getValue();
    s.speed = (float) zspeed.getValue();
    s.start = (float) zstart.getValue();
    s.mode = mode.getSelectedId() == 2 ? pvoc::PlayMode::OneShot : pvoc::PlayMode::Hold;
    s.loop = loop.getToggleState();
    s.posEnv = envOn.getToggleState();
    s.pStart = (float) pStartS.getValue();
    s.pPeak = (float) pPeakS.getValue();
    s.pSustain = (float) pSustainS.getValue();
    s.pEnd = (float) pEndS.getValue();
    s.aMs = (float) aS.getValue();
    s.dMs = (float) dS.getValue();
    s.rMs = (float) rS.getValue();
    s.lfo = lfoOn.getToggleState();
    s.lfoShape = juce::jmax(0, lfoShape.getSelectedId() - 1);
    s.lfoRate = (float) lfoRateS.getValue();
    s.lfoDepth = (float) lfoDepthS.getValue();
    envGraph.setSettings(s);
    proc.updateZone(z->id, s);
}

// ------------------------------------------------------------------ fichiers

void SamplerEditor::openChooser() {
    chooser = std::make_unique<juce::FileChooser>(frs("Ajouter des sons"), juce::File(),
                                                  "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3;*.m4a;*.caf;*.aac");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::canSelectMultipleItems,
                         [this](const juce::FileChooser& fc) {
                             juce::Array<juce::File> files;
                             for (auto& f : fc.getResults()) if (f.existsAsFile()) files.add(f);
                             if (!files.isEmpty()) proc.addFiles(files);
                         });
}

bool SamplerEditor::isInterestedInFileDrag(const juce::StringArray& files) {
    for (auto& f : files) if (juce::File(f).hasFileExtension(audioExt)) return true;
    return false;
}

void SamplerEditor::filesDropped(const juce::StringArray& files, int, int) {
    dragOver = false;
    juce::Array<juce::File> list2;
    for (auto& f : files) {
        const juce::File file(f);
        if (file.isDirectory()) {
            for (auto& c : file.findChildFiles(juce::File::findFiles, false))
                if (c.hasFileExtension(audioExt)) list2.add(c);
        } else if (file.hasFileExtension(audioExt)) {
            list2.add(file);
        }
    }
    list2.sort();
    if (!list2.isEmpty()) proc.addFiles(list2);
    repaint();
}

// ------------------------------------------------------------------ mises a jour

void SamplerEditor::changeListenerCallback(juce::ChangeBroadcaster*) { reloadZones(); }

void SamplerEditor::timerCallback() {
    juce::String st;
    if (proc.isLoading()) st = frs("Chargement…");
    else if (proc.getLastError().isNotEmpty()) st = proc.getLastError();
    else
        st = juce::String(zones.size()) + (zones.size() > 1 ? " sons" : " son") + frs("  ·  voix actives : ")
             + juce::String(proc.getActiveVoices()) + " / "
             + juce::String((int) proc.params.getRawParameterValue("voices")->load());
    statusLabel.setText(st, juce::dontSendNotification);
    positionS.setEnabled(followT.getToggleState());
    positionL.setEnabled(followT.getToggleState());
    clearButton.setVisible(!zones.empty());
}

void SamplerEditor::setTab(int t) {
    tab = t;
    showSelectedZone();
    repaint();
}

// ------------------------------------------------------------------ graphique de l'enveloppe

void PosEnvGraph::paint(juce::Graphics& g) {
    const auto r = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff141b2d));
    g.fillRoundedRectangle(r, 6.0f);
    const auto plot = r.reduced(8.0f, 7.0f).withTrimmedBottom(12.0f).withTrimmedLeft(32.0f);
    const float alpha = s.posEnv ? 1.0f : 0.35f;

    // grille : 0, 50 et 100 % du son
    g.setColour(juce::Colour(0xff35415f).withAlpha(alpha));
    for (float p : { 0.0f, 0.5f, 1.0f }) {
        const float yy = plot.getBottom() - p * plot.getHeight();
        g.drawHorizontalLine((int) yy, plot.getX(), plot.getRight());
    }

    // largeurs : durees reelles pour A, D, R ; maintien de largeur fixe
    const bool oneShot = s.mode == pvoc::PlayMode::OneShot;
    const float a = juce::jmax(0.0f, s.aMs), d = juce::jmax(0.0f, s.dMs), rr = juce::jmax(0.0f, s.rMs);
    const float total = juce::jmax(1.0f, a + d + rr);
    const float sustainW = oneShot ? 0.0f : plot.getWidth() * 0.22f;
    const float scale = (plot.getWidth() - sustainW) / total;
    const float xA = plot.getX() + a * scale, xD = xA + d * scale, xS = xD + sustainW, xR = xS + rr * scale;
    auto yOf = [&](float pct) { return plot.getBottom() - 0.01f * pct * plot.getHeight(); };

    juce::Path p;
    p.startNewSubPath(plot.getX(), yOf(s.pStart));
    p.lineTo(xA, yOf(s.pPeak));
    p.lineTo(xD, yOf(s.pSustain));
    p.lineTo(xS, yOf(s.pSustain));
    p.lineTo(xR, yOf(s.pEnd));

    // LFO : bande autour de la courbe
    if (s.lfo && s.lfoDepth > 0.0f) {
        const float dy = 0.01f * s.lfoDepth * plot.getHeight();
        g.setColour(juce::Colour(0xfff2b84b).withAlpha(0.18f * alpha));
        juce::Path band;
        band.startNewSubPath(plot.getX(), yOf(s.pStart) - dy);
        band.lineTo(xA, yOf(s.pPeak) - dy);
        band.lineTo(xD, yOf(s.pSustain) - dy);
        band.lineTo(xS, yOf(s.pSustain) - dy);
        band.lineTo(xR, yOf(s.pEnd) - dy);
        band.lineTo(xR, yOf(s.pEnd) + dy);
        band.lineTo(xS, yOf(s.pSustain) + dy);
        band.lineTo(xD, yOf(s.pSustain) + dy);
        band.lineTo(xA, yOf(s.pPeak) + dy);
        band.lineTo(plot.getX(), yOf(s.pStart) + dy);
        band.closeSubPath();
        g.fillPath(band);
    }
    g.setColour(juce::Colour(0xfff2b84b).withAlpha(alpha));
    g.strokePath(p, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    for (auto pt : { juce::Point<float>(plot.getX(), yOf(s.pStart)), juce::Point<float>(xA, yOf(s.pPeak)),
                     juce::Point<float>(xD, yOf(s.pSustain)), juce::Point<float>(xS, yOf(s.pSustain)),
                     juce::Point<float>(xR, yOf(s.pEnd)) })
        g.fillEllipse(pt.x - 3.0f, pt.y - 3.0f, 6.0f, 6.0f);

    // lettres des etapes
    g.setColour(juce::Colour(0xffb7c0d8).withAlpha(alpha));
    g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
    auto label = [&](const char* t, float x1, float x2) {
        if (x2 - x1 > 8.0f) g.drawText(t, juce::Rectangle<float>(x1, plot.getBottom() + 1.0f, x2 - x1, 12.0f), juce::Justification::centred);
    };
    label("A", plot.getX(), xA);
    label("D", xA, xD);
    if (!oneShot) label("S", xD, xS);
    label("R", xS, xR);
    // reperes de position a gauche (la courbe commence apres)
    g.setFont(juce::FontOptions(10.0f));
    g.setColour(juce::Colour(0xffb7c0d8).withAlpha(0.7f * alpha));
    g.drawText("100 %", juce::Rectangle<float>(r.getX() + 2.0f, plot.getY() - 6.0f, 34.0f, 12.0f), juce::Justification::centredLeft);
    g.drawText("0 %", juce::Rectangle<float>(r.getX() + 2.0f, plot.getBottom() - 6.0f, 34.0f, 12.0f), juce::Justification::centredLeft);
}
