#include "SamplerEditor.h"

namespace {
const juce::Colour bg(0xff141b2d), header(0xff1f2a44), card(0xff1f2a44), accent(0xfff2b84b),
    text(0xffe8ecf6), textDim(0xffb7c0d8), onAccent(0xff1f2a44), zoneCol(0xff5b6b93);
constexpr int W = 840, Hh = 700;
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
    g.fillRoundedRectangle(16.0f, 112.0f, 290.0f, 300.0f, 12.0f);   // liste
    g.fillRoundedRectangle(318.0f, 112.0f, (float) getWidth() - 334.0f, 300.0f, 12.0f);   // zone
    g.fillRoundedRectangle(16.0f, 424.0f, (float) getWidth() - 32.0f, 150.0f, 12.0f);    // globaux

    if (selectedId == 0) {
        g.setColour(textDim);
        g.setFont(juce::FontOptions(15.0f));
        g.drawText(frs("Sélectionnez un son dans la liste pour régler sa zone."),
                   juce::Rectangle<int>(318, 112, getWidth() - 334, 300), juce::Justification::centred);
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

    list.setBounds(24, 120, 274, 284);
    emptyHint.setBounds(24, 120, 274, 284);

    // zone selectionnee : deux colonnes
    const int x0 = 334, colW = (w - 334 - 32) / 2;
    zoneTitle.setBounds(x0, 118, w - x0 - 170, 26);
    zoneInfo.setBounds(x0, 142, w - x0 - 170, 20);
    removeButton.setBounds(w - 160, 122, 130, 28);
    int y = 166;
    auto noteRow = [&](juce::Label& l, juce::Slider& s, int x) {
        l.setBounds(x, y, colW - 8, 20);
        s.setBounds(x, y + 20, colW - 16, 26);
    };
    noteRow(rootL, root, x0);
    noteRow(tuneL, tune, x0 + colW);
    y += 50;
    noteRow(lowL, low, x0);
    noteRow(highL, high, x0 + colW);
    y += 50;
    modeL.setBounds(x0, y, 60, 26);
    mode.setBounds(x0 + 40, y, colW - 56, 26);
    loop.setBounds(x0 + colW, y, 100, 26);
    y += 32;
    noteRow(releaseL, release, x0);
    noteRow(zgainL, zgain, x0 + colW);
    y += 50;
    zspeedL.setBounds(x0, y, colW - 8, 20);
    zspeed.setBounds(x0, y + 20, colW - 16, 26);
    zstartL.setBounds(x0 + colW, y, colW - 8, 20);
    zstart.setBounds(x0 + colW, y + 20, colW - 16, 26);

    // reglages globaux : 3 colonnes
    const int gx = 30, gw = (w - 60) / 3;
    int gy = 432;
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
                   + (s.loop ? frs("  ·  boucle") : juce::String()),
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
    for (juce::Component* c : std::initializer_list<juce::Component*> {
             &zoneTitle, &zoneInfo, &removeButton, &root, &low, &high, &tune, &release, &zgain, &zspeed, &zstart,
             &rootL, &lowL, &highL, &tuneL, &releaseL, &zgainL, &zspeedL, &zstartL, &mode, &modeL, &loop })
        c->setVisible(has);
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
    release.setEnabled(s.mode == pvoc::PlayMode::Hold);
    releaseL.setEnabled(s.mode == pvoc::PlayMode::Hold);
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
