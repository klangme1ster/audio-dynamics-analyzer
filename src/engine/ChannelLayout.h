#pragma once

#include <array>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace ada
{

// Channel layouts and BS.1770 channel weights (spec 3.11); the same rules as python/ada_core/layout.py.
// A layout is an ordered list of roles, one per input channel. The LFE has weight 0: it is neither
// filtered nor measured. Plain data (no allocation); the string helpers are not for the audio thread.
enum class Role : std::uint8_t { L, R, C, LFE, Ls, Rs, Lb, Rb };

inline constexpr double surroundWeight = 1.41; // +1.5 dB

constexpr double roleWeight (Role r) noexcept
{
    switch (r)
    {
        case Role::LFE: return 0.0;
        case Role::Ls:
        case Role::Rs:
        case Role::Lb:
        case Role::Rb: return surroundWeight;
        default: return 1.0;
    }
}

struct ChannelLayout
{
    static constexpr int maxChannels = 8;

    int count = 0;
    std::array<Role, maxChannels> roles {};

    constexpr ChannelLayout() = default;
    constexpr ChannelLayout (std::initializer_list<Role> list)
    {
        for (auto r : list)
            if (count < maxChannels)
                roles[(std::size_t) count++] = r;
    }

    static constexpr ChannelLayout mono() { return { Role::C }; }
    static constexpr ChannelLayout stereo() { return { Role::L, Role::R }; }
    static constexpr ChannelLayout surround50() { return { Role::L, Role::R, Role::C, Role::Ls, Role::Rs }; }
    static constexpr ChannelLayout surround51() { return { Role::L, Role::R, Role::C, Role::LFE, Role::Ls, Role::Rs }; }
    static constexpr ChannelLayout surround71()
    {
        return { Role::L, Role::R, Role::C, Role::LFE, Role::Lb, Role::Rb, Role::Ls, Role::Rs };
    }

    constexpr bool operator== (const ChannelLayout& o) const noexcept
    {
        if (count != o.count)
            return false;
        for (int i = 0; i < count; ++i)
            if (roles[(std::size_t) i] != o.roles[(std::size_t) i])
                return false;
        return true;
    }

    // 1..8 channels, unique roles, at least one channel besides the LFE
    constexpr bool valid() const noexcept
    {
        if (count < 1 || count > maxChannels)
            return false;
        bool seen[maxChannels] {};
        bool measured = false;
        for (int i = 0; i < count; ++i)
        {
            const auto r = (std::size_t) roles[(std::size_t) i];
            if (seen[r])
                return false;
            seen[r] = true;
            measured = measured || roles[(std::size_t) i] != Role::LFE;
        }
        return measured;
    }

    constexpr double weight (int ch) const noexcept { return roleWeight (roles[(std::size_t) ch]); }

    constexpr int measuredCount() const noexcept
    {
        int n = 0;
        for (int i = 0; i < count; ++i)
            n += weight (i) > 0.0 ? 1 : 0;
        return n;
    }

    // "mono", "stereo", "5.0", "5.1", "7.1" or "custom"
    std::string name() const
    {
        if (*this == mono()) return "mono";
        if (*this == stereo()) return "stereo";
        if (*this == surround50()) return "5.0";
        if (*this == surround51()) return "5.1";
        if (*this == surround71()) return "7.1";
        return "custom";
    }

    std::string roleList() const
    {
        std::string s;
        for (int i = 0; i < count; ++i)
        {
            if (i > 0)
                s += ',';
            s += roleName (roles[(std::size_t) i]);
        }
        return s;
    }

    static constexpr const char* roleName (Role r) noexcept
    {
        constexpr const char* names[] = { "L", "R", "C", "LFE", "Ls", "Rs", "Lb", "Rb" };
        return names[(std::size_t) r];
    }

    static std::optional<ChannelLayout> fromName (std::string_view name)
    {
        if (name == "mono") return mono();
        if (name == "stereo") return stereo();
        if (name == "5.0") return surround50();
        if (name == "5.1") return surround51();
        if (name == "7.1") return surround71();
        return std::nullopt;
    }

    // A standard name, or a comma-separated role list in file channel order ("L,R,C,LFE,Ls,Rs").
    // nullopt for unknown names or roles and for invalid layouts (duplicates, only LFE, > 8 channels).
    static std::optional<ChannelLayout> parse (std::string_view text)
    {
        if (auto named = fromName (text))
            return named;
        ChannelLayout layout;
        while (! text.empty())
        {
            const auto comma = text.find (',');
            const auto token = text.substr (0, comma);
            text = comma == std::string_view::npos ? std::string_view {} : text.substr (comma + 1);
            std::optional<Role> role;
            for (int r = 0; r < maxChannels; ++r)
                if (token == roleName ((Role) r))
                    role = (Role) r;
            if (! role || layout.count >= maxChannels)
                return std::nullopt;
            layout.roles[(std::size_t) layout.count++] = *role;
        }
        return layout.valid() ? std::optional<ChannelLayout> (layout) : std::nullopt;
    }

    // Most plausible layout for a channel count without layout information (5, 6, 8); never applied
    // without the user's confirmation.
    static std::optional<ChannelLayout> suggest (int channels)
    {
        switch (channels)
        {
            case 5: return surround50();
            case 6: return surround51();
            case 8: return surround71();
            default: return std::nullopt;
        }
    }

    // WAVE_FORMAT_EXTENSIBLE dwChannelMask -> standard layout (mono, stereo, 5.0, 5.1, 7.1), else nullopt.
    // A back pair counts as the surround pair unless side channels exist too (0x3F and 0x60F are both 5.1).
    static std::optional<ChannelLayout> fromWavMask (std::uint32_t mask)
    {
        constexpr std::uint32_t known = 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x200 | 0x400;
        if (mask == 0 || (mask & ~known) != 0)
            return std::nullopt;
        const bool side = (mask & 0x200) != 0;
        ChannelLayout layout;
        auto add = [&] (Role r) { layout.roles[(std::size_t) layout.count++] = r; };
        if (mask & 0x1) add (Role::L);
        if (mask & 0x2) add (Role::R);
        if (mask & 0x4) add (Role::C);
        if (mask & 0x8) add (Role::LFE);
        if (mask & 0x10) add (side ? Role::Lb : Role::Ls);
        if (mask & 0x20) add (side ? Role::Rb : Role::Rs);
        if (mask & 0x200) add (Role::Ls);
        if (mask & 0x400) add (Role::Rs);
        for (const auto& s : { mono(), stereo(), surround50(), surround51(), surround71() })
            if (layout == s)
                return s;
        return std::nullopt;
    }
};

} // namespace ada
