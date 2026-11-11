#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "EffectProcessor.h"

/** Spectre en direct : entree (gris) et sortie (jaune). */
class SpectrumView : public juce::Component {
public:
    explicit SpectrumView(const pvoc::SpectralFx& e) : fx(e) {}
    void paint(juce::Graphics&) override;
    void update();
private:
    const pvoc::SpectralFx& fx;
    float inDb[pvoc::SpectralFx::SPEC_BANDS] {}, outDb[pvoc::SpectralFx::SPEC_BANDS] {};
};

class EffectEditor : public juce::AudioProcessorEditor,
                     public juce::FileDragAndDropTarget,
                     private juce::ChangeListener,
                     private juce::Timer {
public:
    explicit EffectEditor(EffectProcessor&);
    ~EffectEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int, int) override;

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override { refresh(); }
    void timerCallback() override;
    void refresh();
    void card(juce::Graphics&, juce::Rectangle<int> r, const juce::String& title);

    EffectProcessor& proc;
    juce::LookAndFeel_V4 lnf;

    juce::TextButton freezeButton;
    juce::ToggleButton pedalT, keepMainT;

    struct Ctl {
        juce::Slider s;
        juce::Label l;
    };
    Ctl pitch, shift, robot, robotHz, whisper, blur, gate, cross, crossSmooth, attack, release, mix, gain, refNote, bend;
    juce::TextButton pm12, p0, pp12, shift0;
    juce::ComboBox crossSource, kbMode;
    juce::Label crossSourceL, kbModeL, modName, modStatus;
    juce::TextButton loadMod, clearMod;

    SpectrumView spectrum;
    juce::MidiKeyboardComponent keyboard;
    std::unique_ptr<juce::FileChooser> chooser;

    using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
    using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    std::vector<std::unique_ptr<SA>> sAtt;
    std::vector<std::unique_ptr<BA>> bAtt;
    std::vector<std::unique_ptr<CA>> cAtt;

    juce::Rectangle<int> rPitch, rSpectral, rCross, rKeys, rOut;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EffectEditor)
};
