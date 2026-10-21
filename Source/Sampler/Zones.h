// Zones du sampler : un son, sa note de base, sa plage de notes et ses reglages.
#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "../PvocEngine.h"

namespace pvoc {

enum class PlayMode { Hold = 0, OneShot = 1 };

/** Reglages d'une zone, modifiables depuis l'interface. */
struct ZoneSettings {
    int root = 60;              // note de base (60 = C3 / Do3)
    int low = 0, high = 127;    // plage de notes couverte
    PlayMode mode = PlayMode::Hold;
    bool loop = false;
    float releaseMs = 200.0f;   // mode tenu : fondu apres le relachement
    float gainDb = 0.0f;
    float speed = 100.0f;       // vitesse (time-stretch) de la zone, en %
    float start = 0.0f;         // point de depart, en % du son
    float tuneCents = 0.0f;     // accord fin
    bool autoRange = true;      // plage calculee automatiquement a partir des notes de base
};

/** Ce que le thread audio voit d'une zone. */
struct ZoneSnapshot {
    uint32_t id = 0;
    ZoneSettings s;
    std::shared_ptr<const PlayerBuffer> buffer;   // son a la frequence du moteur
};

struct ZoneSet {
    std::vector<ZoneSnapshot> zones;
    const ZoneSnapshot* find(uint32_t id) const {
        for (auto& z : zones) if (z.id == id) return &z;
        return nullptr;
    }
};

// ------------------------------------------------------------------ noms de notes

/** 60 -> "C3" (convention de Logic Pro et d'Ableton Live). */
inline juce::String noteName(int n) {
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    n = juce::jlimit(0, 127, n);
    return juce::String(names[n % 12]) + juce::String(n / 12 - 2);
}

/** 60 -> "Do3". */
inline juce::String noteNameFr(int n) {
    static const char* names[] = { "Do", "Do#", "R\xc3\xa9", "R\xc3\xa9#", "Mi", "Fa", "Fa#", "Sol", "Sol#", "La", "La#", "Si" };
    n = juce::jlimit(0, 127, n);
    return juce::String::fromUTF8(names[n % 12]) + juce::String(n / 12 - 2);
}

/**
 * Cherche une note dans un nom de fichier : "Piano_C3.wav", "flute-A#2", "voix Do3", "basse ré#1".
 * Renvoie -1 si rien n'est trouve.
 */
inline int noteFromFileName(const juce::String& fileName) {
    const juce::String s = fileName.upToLastOccurrenceOf(".", false, false).toLowerCase();
    const juce::String fr[] = { "do", juce::String::fromUTF8("r\xc3\xa9"), "re", "mi", "fa", "sol", "la", "si" };
    const int frPc[] = { 0, 2, 2, 4, 5, 7, 9, 11 };
    const int enPc[] = { 9, 11, 0, 2, 4, 5, 7 };   // a b c d e f g
    int best = -1;
    for (int i = 0; i < s.length(); ++i) {
        // la note ne doit pas etre collee a une lettre avant elle
        if (i > 0 && juce::CharacterFunctions::isLetter(s[i - 1])) continue;
        int pc = -1, len = 0;
        for (int k = 0; k < 8 && pc < 0; ++k)
            if (s.substring(i).startsWith(fr[k])) { pc = frPc[k]; len = fr[k].length(); }
        if (pc < 0 && s[i] >= 'a' && s[i] <= 'g') { pc = enPc[s[i] - 'a']; len = 1; }
        if (pc < 0) continue;
        int j = i + len;
        if (j < s.length() && (s[j] == '#' || s[j] == 's')) { ++pc; ++j; }
        else if (j < s.length() && s[j] == 'b' && j + 1 < s.length()
                 && (juce::CharacterFunctions::isDigit(s[j + 1]) || s[j + 1] == '-')) { --pc; ++j; }
        bool neg = false;
        if (j < s.length() && s[j] == '-') { neg = true; ++j; }
        if (j >= s.length() || !juce::CharacterFunctions::isDigit(s[j])) continue;
        const int octave = s[j] - '0';
        if (j + 1 < s.length() && juce::CharacterFunctions::isDigit(s[j + 1])) continue;   // "c10" n'est pas une note
        const int note = (neg ? -octave : octave) * 12 + 24 + pc;
        if (note >= 0 && note <= 127) best = note;   // la derniere note trouvee l'emporte
    }
    return best;
}

/** Repartit les plages des zones "automatiques" entre leurs notes de base (milieux). */
template <typename ZoneT>
inline void spreadRanges(std::vector<ZoneT*>& zs, bool all) {
    std::vector<ZoneT*> v;
    for (auto* z : zs) if (all || z->settings.autoRange) v.push_back(z);
    std::stable_sort(v.begin(), v.end(), [](ZoneT* a, ZoneT* b) { return a->settings.root < b->settings.root; });
    for (size_t i = 0; i < v.size(); ++i) {
        auto& s = v[i]->settings;
        // zones de meme note de base : meme plage (superposition)
        size_t a = i, b = i;
        while (a > 0 && v[a - 1]->settings.root == s.root) --a;
        while (b + 1 < v.size() && v[b + 1]->settings.root == s.root) ++b;
        s.low = a == 0 ? 0 : (v[a - 1]->settings.root + s.root) / 2 + 1;
        s.high = b + 1 >= v.size() ? 127 : (s.root + v[b + 1]->settings.root) / 2;
        if (all) s.autoRange = true;
    }
}

} // namespace pvoc
