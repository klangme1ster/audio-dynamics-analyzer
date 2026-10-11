#pragma once

#include <juce_graphics/juce_graphics.h>

#include <cmath>

// Dark theme shared by all views (values from the project's data-viz palette, dark mode).
namespace theme
{
inline const juce::Colour background { 0xff1a1a19 };
inline const juce::Colour panel { 0xff222221 };
inline const juce::Colour grid { 0xff383835 };
inline const juce::Colour textPrimary { 0xfff0efec };
inline const juce::Colour textSecondary { 0xffc3c2b7 };
inline const juce::Colour textMuted { 0xff8f8e88 };
inline const juce::Colour shortTerm { 0xff3987e5 }; // categorical slot 1 (dark)
inline const juce::Colour truePeak { 0xffd95926 }; // categorical slot 2 (dark)
inline const juce::Colour psrLine { 0xfff0efec };

// PSR zones (status palette): green >= 12, yellow 10-12, orange 8-10, red < 8 dB
inline const juce::Colour zoneColours[4] { juce::Colour (0xff0ca30c), juce::Colour (0xfffab219),
                                           juce::Colour (0xffec835a), juce::Colour (0xffd03b3b) };
inline const char* const zoneNames[4] { "Open", "Controlled", "Heavy", "Over-limited" };
inline juce::String zoneRange (int zone)
{
    static const char* const ranges[4] { "\xe2\x89\xa5 12", "10\xe2\x80\x93" "12", "8\xe2\x80\x93" "10", "< 8" };
    return juce::String (juce::CharPointer_UTF8 (ranges[zone]));
}
inline constexpr float zoneLower[4] { 12.0f, 10.0f, 8.0f, -1.0e9f };
inline constexpr float zoneUpper[4] { 1.0e9f, 12.0f, 10.0f, 8.0f };
inline constexpr float psrRangeMin = 0.0f, psrRangeMax = 24.0f;

inline int zoneOf (float psr) { return psr >= 12.0f ? 0 : psr >= 10.0f ? 1 : psr >= 8.0f ? 2 : 3; }

inline juce::String minusSign() { return juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92")); }
inline juce::String number (float v, int decimals = 1)
{
    // juce::String (float, 0) means "full precision", so whole numbers are rounded explicitly
    const auto text = decimals == 0 ? juce::String (juce::roundToInt (v)) : juce::String (v, decimals);
    return text.replace ("-", minusSign());
}
inline juce::String dash() { return juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x93")); }

inline juce::String timeLabel (double seconds, bool tenths = false)
{
    // round to the displayed resolution first, so 119.97 s reads 2:00.0 rather than 1:60.0
    const double unit = tenths ? 10.0 : 1.0;
    const auto ticks = (long long) std::llround (std::abs (seconds) * unit);
    const long long perMinute = (long long) (60.0 * unit);
    const auto m = ticks / perMinute;
    const auto rest = ticks % perMinute;
    const juce::String sign = seconds < 0.0 && ticks > 0 ? minusSign() : juce::String();
    return sign + (tenths ? juce::String::formatted ("%lld:%02lld.%lld", m, rest / 10, rest % 10)
                          : juce::String::formatted ("%lld:%02lld", m, rest));
}
} // namespace theme
