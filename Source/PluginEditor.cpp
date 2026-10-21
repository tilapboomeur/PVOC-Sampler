#include "PluginEditor.h"

namespace {
const juce::Colour bg(0xff141b2d), header(0xff1f2a44), card(0xff1f2a44), accent(0xfff2b84b),
    text(0xffe8ecf6), textDim(0xffb7c0d8), onAccent(0xff1f2a44);
constexpr int W = 560, Hh = 706;
}

PvocEditor::PvocEditor(PvocProcessor& p) : AudioProcessorEditor(p), proc(p) {
    lnf.setColourScheme({ bg, header, card, textDim, text, accent, onAccent, header, text });
    lnf.setColour(juce::Slider::thumbColourId, accent);
    lnf.setColour(juce::Slider::trackColourId, accent.withAlpha(0.8f));
    lnf.setColour(juce::Slider::backgroundColourId, juce::Colour(0xff35415f));
    lnf.setColour(juce::ToggleButton::tickColourId, accent);
    lnf.setColour(juce::TextButton::buttonOnColourId, accent);
    lnf.setColour(juce::TextButton::textColourOnId, onAccent);
    setLookAndFeel(&lnf);

    // ----- bouton principal
    openButton.setButtonText(fr("Ouvrir un son…"));
    openButton.setColour(juce::TextButton::buttonColourId, accent);
    openButton.setColour(juce::TextButton::textColourOffId, onAccent);
    openButton.onClick = [this] { openChooser(); };
    addAndMakeVisible(openButton);

    fileLabel.setFont(juce::FontOptions(17.0f, juce::Font::bold));
    fileLabel.setColour(juce::Label::textColourId, text);
    infoLabel.setColour(juce::Label::textColourId, textDim);
    timeLabel.setColour(juce::Label::textColourId, textDim);
    durLabel.setColour(juce::Label::textColourId, textDim);
    durLabel.setJustificationType(juce::Justification::centredRight);
    for (auto* l : { &fileLabel, &infoLabel, &timeLabel, &durLabel }) addAndMakeVisible(*l);

    position.setSliderStyle(juce::Slider::LinearHorizontal);
    position.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    position.setRange(0.0, 1.0, 0.0);
    position.onDragStart = [this] {
        userDragging = true;
        if (proc.followEnabled()) {
            // en mode pilote, le curseur ecrit le parametre "Position" (enregistrable en automation)
            if (auto* prm = proc.params.getParameter("position")) {
                prm->beginChangeGesture();
                prm->setValueNotifyingHost((float) position.getValue());
            }
        }
        if (proc.scrubEnabled()) proc.beginScrub(position.getValue());
    };
    position.onDragEnd = [this] {
        if (proc.followEnabled()) {
            if (auto* prm = proc.params.getParameter("position")) {
                prm->setValueNotifyingHost((float) position.getValue());
                prm->endChangeGesture();
            }
        }
        if (proc.scrubEnabled()) proc.endScrub(position.getValue());
        else if (!proc.followEnabled()) proc.requestSeek(position.getValue());
        userDragging = false;
    };
    position.onValueChange = [this] {
        if (!userDragging) return;
        if (proc.followEnabled())
            if (auto* prm = proc.params.getParameter("position")) prm->setValueNotifyingHost((float) position.getValue());
        if (proc.scrubEnabled()) proc.moveScrub(position.getValue());
        updateTimeLabels(position.getValue());
    };
    addAndMakeVisible(position);

    // ----- mode
    for (auto* b : { &modePlayer, &modeEffect }) {
        b->setClickingTogglesState(false);
        b->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2a3654));
        addAndMakeVisible(*b);
    }
    modePlayer.setButtonText("Lecteur (fichier)");
    modeEffect.setButtonText(fr("Effet (piste en direct)"));
    modePlayer.setConnectedEdges(juce::Button::ConnectedOnRight);
    modeEffect.setConnectedEdges(juce::Button::ConnectedOnLeft);
    modePlayer.onClick = [this] { setMode(0); };
    modeEffect.onClick = [this] { setMode(1); };

    // ----- transport
    playButton.setButtonText("Lecture");
    playButton.onClick = [this] { if (proc.isPlayingNow()) proc.requestPause(); else proc.requestPlay(); };
    stopButton.setButtonText("Stop");
    stopButton.onClick = [this] { proc.requestStop(); };
    addAndMakeVisible(playButton);
    addAndMakeVisible(stopButton);

    loopToggle.setButtonText("Boucle");
    reverseToggle.setButtonText(fr("À l'envers"));
    syncToggle.setButtonText("Suivre le transport du DAW");
    lockToggle.setButtonText("Verrouillage de phase (son plus net)");
    freezeToggle.setButtonText("Gel du son");
    followToggle.setButtonText(fr("Position pilotée par automation / MIDI"));
    followToggle.setTooltip(fr("La tête de lecture suit le paramètre « Position » du DAW "
                               "(automation ou contrôleur MIDI). La vitesse est alors ignorée."));
    addAndMakeVisible(followToggle);
    scrubToggle.setButtonText("Scrub");
    scrubToggle.setTooltip(fr("Le son suit le curseur de position pendant qu'on le déplace "
                              "(lecteur, ou effet avec gel du son)."));
    for (auto* t : { &loopToggle, &reverseToggle, &syncToggle, &lockToggle, &freezeToggle, &scrubToggle }) addAndMakeVisible(*t);

    auto setupSlider = [this](juce::Slider& s, juce::Label& l, const juce::String& name, const juce::String& suffix) {
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 90, 22);
        s.setTextValueSuffix(suffix);
        l.setText(name, juce::dontSendNotification);
        l.setColour(juce::Label::textColourId, text);
        addAndMakeVisible(s);
        addAndMakeVisible(l);
    };
    setupSlider(speed, speedLabel, "Vitesse (time-stretch)", " %");
    setupSlider(transpose, transposeLabel, "Hauteur (pitch-shift)", " demi-tons");
    setupSlider(mix, mixLabel, fr("Mélange sec / traité"), " %");
    setupSlider(gain, gainLabel, "Volume", " dB");

    auto& ps = proc.params;
    aSpeed = std::make_unique<SA>(ps, "speed", speed);
    aTranspose = std::make_unique<SA>(ps, "transpose", transpose);
    aMix = std::make_unique<SA>(ps, "mix", mix);
    aGain = std::make_unique<SA>(ps, "gain", gain);
    aLoop = std::make_unique<BA>(ps, "loop", loopToggle);
    aReverse = std::make_unique<BA>(ps, "reverse", reverseToggle);
    aSync = std::make_unique<BA>(ps, "sync", syncToggle);
    aLock = std::make_unique<BA>(ps, "lock", lockToggle);
    aFreeze = std::make_unique<BA>(ps, "freeze", freezeToggle);
    aScrub = std::make_unique<BA>(ps, "scrub", scrubToggle);
    aFollow = std::make_unique<BA>(ps, "follow", followToggle);
    smoothLabel.setText("Lissage", juce::dontSendNotification);
    smoothLabel.setColour(juce::Label::textColourId, text);
    smoothLabel.setTooltip(fr("Temps de glissement de la position (scrub, automation, MIDI). 0 ms = saut immédiat."));
    smooth.setSliderStyle(juce::Slider::LinearHorizontal);
    smooth.setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, 22);
    smooth.setTextValueSuffix(" ms");
    smooth.setTooltip(smoothLabel.getTooltip());
    addAndMakeVisible(smoothLabel);
    addAndMakeVisible(smooth);
    aSmooth = std::make_unique<SA>(ps, "smooth", smooth);
    smooth.setNumDecimalPlacesToDisplay(0);
    speed.setNumDecimalPlacesToDisplay(0);
    transpose.setNumDecimalPlacesToDisplay(2);
    mix.setNumDecimalPlacesToDisplay(0);
    gain.setNumDecimalPlacesToDisplay(1);

    auto quick = [this](juce::TextButton& b, const juce::String& t, juce::Slider& s, double v) {
        b.setButtonText(t);
        b.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2a3654));
        b.onClick = [&s, v] { s.setValue(v, juce::sendNotificationSync); };
        addAndMakeVisible(b);
    };
    quick(sp50, "50 %", speed, 50.0);
    quick(sp100, "100 %", speed, 100.0);
    quick(sp200, "200 %", speed, 200.0);
    quick(trMinus12, fr("−12"), transpose, -12.0);
    quick(tr0, "0", transpose, 0.0);
    quick(trPlus12, "+12", transpose, 12.0);

    proc.addChangeListener(this);
    setSize(W, Hh);
    refresh();
    startTimerHz(30);
}

PvocEditor::~PvocEditor() {
    proc.removeChangeListener(this);
    setLookAndFeel(nullptr);
}

void PvocEditor::paint(juce::Graphics& g) {
    g.fillAll(bg);
    g.setColour(header);
    g.fillRect(0, 0, getWidth(), 118);
    g.setColour(text);
    g.setFont(juce::FontOptions(22.0f, juce::Font::bold));
    g.drawText("Lecteur PVoc", 16, 10, 300, 28, juce::Justification::centredLeft);
    g.setColour(textDim);
    g.setFont(juce::FontOptions(14.0f));
    g.drawText(fr("Vocodeur de phase · time-stretch et pitch-shift indépendants"), 16, 36, getWidth() - 32, 20,
               juce::Justification::centredLeft);
    g.setColour(card);
    g.fillRoundedRectangle(16.0f, 130.0f, (float) getWidth() - 32.0f, 170.0f, 12.0f);
    if (dragOver) {
        g.setColour(accent);
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(3.0f), 12.0f, 3.0f);
    }
}

void PvocEditor::resized() {
    const int w = getWidth();
    openButton.setBounds(16, 62, w - 32, 44);

    fileLabel.setBounds(28, 138, w - 56 - 100, 24);
    scrubToggle.setBounds(w - 124, 136, 96, 28);
    infoLabel.setBounds(28, 160, w - 56, 20);
    position.setBounds(24, 182, w - 48, 24);
    timeLabel.setBounds(28, 206, 200, 20);
    durLabel.setBounds(w - 228, 206, 200, 20);
    followToggle.setBounds(24, 230, w - 48, 28);
    smoothLabel.setBounds(28, 264, 70, 24);
    smooth.setBounds(96, 263, w - 120, 26);

    modePlayer.setBounds(16, 312, (w - 32) / 2, 34);
    modeEffect.setBounds(16 + (w - 32) / 2, 312, (w - 32) / 2, 34);

    playButton.setBounds(16, 358, 120, 32);
    stopButton.setBounds(144, 358, 90, 32);
    loopToggle.setBounds(248, 358, 100, 32);
    reverseToggle.setBounds(350, 358, 120, 32);
    syncToggle.setBounds(16, 392, 260, 28);
    freezeToggle.setBounds(290, 392, 200, 28);

    int y = 428;
    auto row = [&](juce::Label& l, juce::Slider& s) {
        l.setBounds(16, y, w - 32, 20);
        s.setBounds(16, y + 20, w - 32, 26);
        y += 50;
    };
    row(speedLabel, speed);
    sp50.setBounds(16, y - 2, 70, 24); sp100.setBounds(92, y - 2, 70, 24); sp200.setBounds(168, y - 2, 70, 24);
    y += 28;
    row(transposeLabel, transpose);
    trMinus12.setBounds(16, y - 2, 70, 24); tr0.setBounds(92, y - 2, 70, 24); trPlus12.setBounds(168, y - 2, 70, 24);
    lockToggle.setBounds(250, y - 4, w - 266, 28);
    y += 28;
    row(mixLabel, mix);
    gainLabel.setBounds(16, y, 80, 20);
    gain.setBounds(16, y + 20, w - 32, 26);
}

void PvocEditor::openChooser() {
    chooser = std::make_unique<juce::FileChooser>(fr("Choisir un son"), juce::File(),
                                                  "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3;*.m4a;*.caf;*.aac");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                         [this](const juce::FileChooser& fc) {
                             const auto f = fc.getResult();
                             if (f.existsAsFile()) proc.loadFile(f);
                         });
}

bool PvocEditor::isInterestedInFileDrag(const juce::StringArray& files) {
    for (auto& f : files)
        if (juce::File(f).hasFileExtension("wav;aif;aiff;flac;ogg;mp3;m4a;caf;aac")) return true;
    return false;
}

void PvocEditor::filesDropped(const juce::StringArray& files, int, int) {
    dragOver = false;
    repaint();
    for (auto& f : files) {
        const juce::File file(f);
        if (file.hasFileExtension("wav;aif;aiff;flac;ogg;mp3;m4a;caf;aac")) { proc.loadFile(file); break; }
    }
}

void PvocEditor::setMode(int m) {
    if (auto* prm = proc.params.getParameter("mode")) {
        prm->beginChangeGesture();
        prm->setValueNotifyingHost(prm->convertTo0to1((float) m));
        prm->endChangeGesture();
    }
    refresh();
}

void PvocEditor::changeListenerCallback(juce::ChangeBroadcaster*) { refresh(); }

void PvocEditor::timerCallback() {
    if (!userDragging) {
        const bool effect = proc.params.getRawParameterValue("mode")->load() > 0.5f;
        const bool frozen = proc.params.getRawParameterValue("freeze")->load() > 0.5f;
        const double pos = proc.followEnabled() && (!effect || frozen)
                               ? proc.params.getRawParameterValue("position")->load() / 100.0
                               : proc.getPosition();
        position.setValue(pos, juce::dontSendNotification);
        updateTimeLabels(pos);
    }
    playButton.setButtonText(proc.isPlayingNow() ? "Pause" : "Lecture");
    refresh();
}

void PvocEditor::refresh() {
    const bool effect = proc.params.getRawParameterValue("mode")->load() > 0.5f;
    modePlayer.setToggleState(!effect, juce::dontSendNotification);
    modeEffect.setToggleState(effect, juce::dontSendNotification);

    const bool loading = proc.isLoading();
    const auto name = proc.getFileName();
    const auto err = proc.getLastError();
    openButton.setEnabled(!loading);
    openButton.setButtonText(loading ? fr("Chargement…") : (name.isEmpty() ? fr("Ouvrir un son…") : fr("Ouvrir un autre son…")));
    fileLabel.setText(name.isEmpty() ? fr("Aucun son chargé") : name, juce::dontSendNotification);
    if (err.isNotEmpty()) infoLabel.setText(err, juce::dontSendNotification);
    else if (name.isEmpty()) infoLabel.setText(fr("Cliquez sur « Ouvrir un son… » ou glissez un fichier audio ici."), juce::dontSendNotification);
    else infoLabel.setText(proc.getFileInfo(), juce::dontSendNotification);
    const bool hasFile = name.isNotEmpty() && !loading;
    const bool frozen = proc.params.getRawParameterValue("freeze")->load() > 0.5f;
    for (juce::Component* c : std::initializer_list<juce::Component*> { &playButton, &stopButton, &loopToggle,
                                                                       &reverseToggle, &syncToggle, &speed, &speedLabel,
                                                                       &sp50, &sp100, &sp200 })
        c->setEnabled(!effect && (hasFile || c == &syncToggle || c == &loopToggle || c == &reverseToggle
                                  || c == &speed || c == &speedLabel || c == &sp50 || c == &sp100 || c == &sp200));
    for (juce::Component* c : std::initializer_list<juce::Component*> { &freezeToggle, &mix, &mixLabel })
        c->setEnabled(effect);
    // le curseur de position sert au fichier (lecteur) ou a l'historique du gel (effet)
    position.setEnabled(effect ? frozen : hasFile);
    scrubToggle.setEnabled(effect ? frozen : hasFile);
    // en mode pilote, la vitesse n'a plus d'effet
    const bool follow = proc.followEnabled();
    for (juce::Component* c : std::initializer_list<juce::Component*> { &speed, &speedLabel, &sp50, &sp100, &sp200, &loopToggle, &reverseToggle })
        if (follow) c->setEnabled(false);
}

void PvocEditor::updateTimeLabels(double pos) {
    const bool effect = proc.params.getRawParameterValue("mode")->load() > 0.5f;
    if (!effect) {
        timeLabel.setText(formatTime(pos * proc.getDurationSeconds()), juce::dontSendNotification);
        durLabel.setText(formatTime(proc.getDurationSeconds()), juce::dontSendNotification);
    } else if (proc.params.getRawParameterValue("freeze")->load() > 0.5f) {
        const double back = (1.0 - pos) * proc.getHistorySeconds();
        timeLabel.setText(back < 0.05 ? fr("instant du gel") : fr("−") + juce::String(back, 1) + " s",
                          juce::dontSendNotification);
        durLabel.setText(fr("gel"), juce::dontSendNotification);
    } else {
        timeLabel.setText(fr("en direct"), juce::dontSendNotification);
        durLabel.setText(fr("activez le gel pour voyager"), juce::dontSendNotification);
    }
}

juce::String PvocEditor::formatTime(double s) {
    const int t = juce::jmax(0, (int) s);
    return juce::String(t / 60) + ":" + juce::String(t % 60).paddedLeft('0', 2);
}
