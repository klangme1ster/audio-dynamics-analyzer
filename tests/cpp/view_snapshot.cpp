// ada_view_snapshot: developer tool that renders the plugin's dynamics views (history graph, PSR
// meter, zone bar) for a synthetic song into a PNG, to inspect the GUI without a host.
//
//   ada_view_snapshot <out.png> [width] [height]

#include "DynamicsEngine.h"
#include "DynamicsViews.h"
#include "HistoryGraph.h"
#include "HistoryStore.h"
#include "Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <random>
#include <vector>

namespace
{
struct StoreSink : ada::MetricsSink
{
    ada::HistoryStore& store;
    explicit StoreSink (ada::HistoryStore& s) : store (s) {}
    void push (const ada::DynamicsMetrics& f) noexcept override { store.push (f); }
};

// 2 min "song": 30 s dynamic intro, 60 s heavily limited chorus, 30 s quieter outro
void renderSong (ada::HistoryStore& store)
{
    const double fs = 48000.0;
    ada::DynamicsEngine engine;
    engine.prepare (fs, 2);
    StoreSink sink (store);
    std::mt19937 rng (5);
    std::normal_distribution<float> noise (0.0f, 1.0f);
    std::vector<float> l (4800), r (4800);
    const float* ch[2] = { l.data(), r.data() };
    std::int64_t n = 0;
    for (int block = 0; block < 1200; ++block)
    {
        for (int i = 0; i < 4800; ++i, ++n)
        {
            const double t = (double) n / fs;
            const double beat = std::fmod (t, 0.5);
            const double kick = 0.9 * std::exp (-beat / 0.02) * std::sin (2.0 * 3.14159265 * 55.0 * beat);
            const double bed = 0.12 * std::sin (2.0 * 3.14159265 * 110.0 * t) + 0.04 * noise (rng);
            double x = (kick + bed) * (t < 30.0 ? 0.6 : t < 90.0 ? 1.0 : 0.45);
            if (t >= 30.0 && t < 90.0)
                x = std::tanh (5.0 * x) / std::tanh (5.0) * 0.97; // brickwall-ish limiting
            l[(size_t) i] = (float) x;
            r[(size_t) i] = (float) (x * 0.97);
        }
        engine.process (ch, 4800, sink);
    }
}

class Snapshot : public juce::Component
{
public:
    explicit Snapshot (const ada::HistoryStore& s) : graph (s)
    {
        addAndMakeVisible (graph);
        addAndMakeVisible (meter);
        addAndMakeVisible (zoneBar);
        addAndMakeVisible (targetPanel);
        const auto stats = s.psrStats();
        const auto& f = s.latest();
        meter.setValues (ada::passesLivePsrGate (f) ? std::optional<float> (f.psr) : std::nullopt, stats.min, stats.p10);
        zoneBar.setStats (stats);

        // delivery target: Spotify, measured against the song's integrated loudness and max true peak
        const auto& spotify = ada::deliveryTargets[0];
        targetPanel.setSelection (0, spotify);
        auto finite = [] (float v) { return std::isfinite (v) ? std::optional<double> (v) : std::nullopt; };
        targetPanel.setMeasurement (finite (f.integratedLUFS), finite (f.truePeakMaxDBTP));
        const auto verdict = targetPanel.verdict();
        graph.setTargetLines ((float) spotify.loudness, verdict ? std::optional<float> ((float) verdict->ceiling) : std::nullopt);
    }
    void paint (juce::Graphics& g) override { g.fillAll (theme::background); }
    void resized() override
    {
        auto area = getLocalBounds().reduced (8);
        auto bottom = area.removeFromBottom (72);
        targetPanel.setBounds (bottom.removeFromRight (170));
        bottom.removeFromRight (8);
        zoneBar.setBounds (bottom);
        area.removeFromBottom (8);
        meter.setBounds (area.removeFromRight (170));
        area.removeFromRight (8);
        graph.setBounds (area);
    }
    HistoryGraph graph;
    PsrMeter meter;
    ZoneBar zoneBar;
    TargetPanel targetPanel;
};
} // namespace

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf (stderr, "usage: ada_view_snapshot <out.png> [width] [height]\n");
        return 2;
    }
    juce::ScopedJuceInitialiser_GUI gui;
    ada::HistoryStore store;
    renderSong (store);

    Snapshot view (store);
    view.setSize (argc > 2 ? std::atoi (argv[2]) : 980, argc > 3 ? std::atoi (argv[3]) : 520);
    const auto image = view.createComponentSnapshot (view.getLocalBounds(), true, 1.0f);
    juce::File out (juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]));
    out.deleteFile();
    juce::FileOutputStream stream (out);
    juce::PNGImageFormat png;
    if (! stream.openedOk() || ! png.writeImageToStream (image, stream))
    {
        std::fprintf (stderr, "could not write %s\n", argv[1]);
        return 1;
    }
    std::printf ("wrote %s (%d frames)\n", out.getFullPathName().toRawUTF8(), store.size());
    return 0;
}
