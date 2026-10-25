#pragma once

#include "ChannelLayout.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <optional>
#include <string>

namespace ada
{

// A host bus layout as roles in buffer channel order (JUCE orders a set's channels by channel
// type), or nullopt for sets that are not mono, stereo, 5.0, 5.1 or 7.1 in any speaker order
// (quad, LCR, discrete channels, immersive formats).
inline std::optional<ChannelLayout> layoutFromChannelSet (const juce::AudioChannelSet& set)
{
    using T = juce::AudioChannelSet;
    if (set == T::mono())
        return ChannelLayout::mono();
    const auto types = set.getChannelTypes();
    const bool side = set.getChannelIndexForType (T::leftSurroundSide) >= 0;
    ChannelLayout layout;
    for (auto type : types)
    {
        Role role;
        switch (type)
        {
            case T::left: role = Role::L; break;
            case T::right: role = Role::R; break;
            case T::centre: role = Role::C; break;
            case T::LFE: role = Role::LFE; break;
            case T::leftSurround: role = side ? Role::Lb : Role::Ls; break;
            case T::rightSurround: role = side ? Role::Rb : Role::Rs; break;
            case T::leftSurroundSide: role = Role::Ls; break;
            case T::rightSurroundSide: role = Role::Rs; break;
            case T::leftSurroundRear: role = Role::Lb; break;
            case T::rightSurroundRear: role = Role::Rb; break;
            default: return std::nullopt;
        }
        if (layout.count >= ChannelLayout::maxChannels)
            return std::nullopt;
        layout.roles[(std::size_t) layout.count++] = role;
    }
    // must be one of the standard speaker sets, possibly in another order
    for (const auto& standard : { ChannelLayout::stereo(), ChannelLayout::surround50(), ChannelLayout::surround51(),
                                  ChannelLayout::surround71() })
    {
        auto a = layout.roles, b = standard.roles;
        std::sort (a.begin(), a.begin() + layout.count);
        std::sort (b.begin(), b.begin() + standard.count);
        if (layout.count == standard.count && std::equal (a.begin(), a.begin() + layout.count, b.begin()))
            return layout;
    }
    return std::nullopt;
}

// "5.1" for a layout that is a standard one in another channel order too; else ChannelLayout::name()
inline std::string displayName (const ChannelLayout& layout)
{
    for (const auto& standard : { ChannelLayout::mono(), ChannelLayout::stereo(), ChannelLayout::surround50(),
                                  ChannelLayout::surround51(), ChannelLayout::surround71() })
    {
        auto a = layout.roles, b = standard.roles;
        std::sort (a.begin(), a.begin() + layout.count);
        std::sort (b.begin(), b.begin() + standard.count);
        if (layout.count == standard.count && std::equal (a.begin(), a.begin() + layout.count, b.begin()))
            return standard.name();
    }
    return layout.name();
}

} // namespace ada
