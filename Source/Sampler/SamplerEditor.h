#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "SamplerProcessor.h"

class SamplerEditor : public juce::AudioProcessorEditor,
                      public juce::FileDragAndDropTarget,
                      private juce::ListBoxModel,
                      private juce::ChangeListener,
                      private juce::Timer {
public:
    explicit SamplerEditor(SamplerProcessor&);
    ~SamplerEditor() override;

    void paint(juce::Graphics&) override;
    void paintOverChildren(juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragEnter(const juce::StringArray&, int, int) override { dragOver = true; repaint(); }
    void fileDragExit(const juce::StringArray&) override { dragOver = false; repaint(); }
    void filesDropped(const juce::StringArray& files, int, int) override;

    /** Pour les tests : selectionne une zone par son rang. */
    void selectRow(int row) { list.selectRow(row); }

private:
    // ListBoxModel
    int getNumRows() override { return (int) zones.size(); }
    void paintListBoxItem(int row, juce::Graphics&, int w, int h, bool selected) override;
    void selectedRowsChanged(int lastRowSelected) override;
    void deleteKeyPressed(int lastRowSelected) override;

    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    void reloadZones();
    void showSelectedZone();
    void pushZoneEdit();
    void openChooser();
    const SamplerProcessor::ZoneInfo* selectedZone() const;

    SamplerProcessor& proc;
    juce::LookAndFeel_V4 lnf;
    std::vector<SamplerProcessor::ZoneInfo> zones;
    uint32_t selectedId = 0;
    bool updatingUi = false;
    bool dragOver = false;

    juce::TextButton addButton, spreadButton, removeButton, clearButton;
    juce::ListBox list { "zones", this };
    juce::Label statusLabel, zoneTitle, zoneInfo, emptyHint;

    // edition de la zone selectionnee
    juce::Slider root, low, high, tune, release, zgain, zspeed, zstart;
    juce::Label rootL, lowL, highL, tuneL, releaseL, zgainL, zspeedL, zstartL, modeL;
    juce::ComboBox mode;
    juce::ToggleButton loop;

    // reglages globaux (parametres automatisables)
    juce::Slider voicesS, speedS, transposeS, velocityS, bendS, gainS, positionS, smoothS;
    juce::Label voicesL, speedL, transposeL, velocityL, bendL, gainL, positionL, smoothL;
    juce::ToggleButton followT, lockT;
    using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
    using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::vector<std::unique_ptr<SA>> sliderAtt;
    std::vector<std::unique_ptr<BA>> buttonAtt;

    juce::MidiKeyboardComponent keyboard;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SamplerEditor)
};
