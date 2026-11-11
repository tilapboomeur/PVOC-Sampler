#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"

class PvocEditor : public juce::AudioProcessorEditor,
                   public juce::FileDragAndDropTarget,
                   private juce::ChangeListener,
                   private juce::Timer {
public:
    explicit PvocEditor(PvocProcessor&);
    ~PvocEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragEnter(const juce::StringArray&, int, int) override { dragOver = true; repaint(); }
    void fileDragExit(const juce::StringArray&) override { dragOver = false; repaint(); }
    void filesDropped(const juce::StringArray& files, int, int) override;

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void openChooser();
    void setMode(int m);
    void refresh();
    void updateTimeLabels(double pos);
    static juce::String formatTime(double s);

    PvocProcessor& proc;
    juce::LookAndFeel_V4 lnf;

    juce::TextButton openButton, modePlayer, modeEffect, playButton, stopButton;
    juce::ToggleButton loopToggle, reverseToggle, syncToggle, lockToggle, freezeToggle, scrubToggle, followToggle;
    juce::Slider position, speed, transpose, mix, gain, smooth;
    juce::Label fileLabel, infoLabel, timeLabel, durLabel;
    juce::Label speedLabel, transposeLabel, mixLabel, gainLabel, smoothLabel;
    juce::TextButton sp50, sp100, sp200, tr0, trMinus12, trPlus12;

    using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
    using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::unique_ptr<SA> aSpeed, aTranspose, aMix, aGain, aSmooth;
    std::unique_ptr<BA> aLoop, aReverse, aSync, aLock, aFreeze, aScrub, aFollow;

    std::unique_ptr<juce::FileChooser> chooser;
    bool dragOver = false;
    bool userDragging = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PvocEditor)
};
