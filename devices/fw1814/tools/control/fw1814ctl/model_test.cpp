#include "../../../special_mixer_model.h"

#include <cassert>

int main() {
    using Model = macfw::fw1814::SpecialMixerRoutingModel;

    Model model;
    model.loadStraightAnalogPlaybackPreset();
    assert(model.mixStreamIn() == 0x00000006u);
    assert(model.srcAnalogOut() == 0x00000000u);
    assert(model.isStraightAnalogPlaybackPreset());

    assert(model.streamRoute(Model::StreamSource::Stream12,
                             Model::MixerBus::Mixer12));
    assert(!model.streamRoute(Model::StreamSource::Stream12,
                              Model::MixerBus::Mixer34));
    assert(!model.streamRoute(Model::StreamSource::Stream34,
                              Model::MixerBus::Mixer12));
    assert(model.streamRoute(Model::StreamSource::Stream34,
                             Model::MixerBus::Mixer34));

    model.setStreamRoute(Model::StreamSource::Stream12,
                         Model::MixerBus::Mixer34, true);
    assert(model.mixStreamIn() == 0x0000000eu);
    model.setStreamRoute(Model::StreamSource::Stream34,
                         Model::MixerBus::Mixer34, false);
    assert(model.mixStreamIn() == 0x0000000cu);

    model.setAnalogOutputSource(Model::AnalogOutputPair::Analog12,
                                Model::OutputSource::Aux);
    assert(model.srcAnalogOut() == 0x00000001u);
    assert(model.analogOutputSource(Model::AnalogOutputPair::Analog12) ==
           Model::OutputSource::Aux);
    assert(model.analogOutputSource(Model::AnalogOutputPair::Analog34) ==
           Model::OutputSource::Mixer);

    model.setAnalogOutputSource(Model::AnalogOutputPair::Analog34,
                                Model::OutputSource::Aux);
    assert(model.srcAnalogOut() == 0x00000003u);
    model.setAnalogOutputSource(Model::AnalogOutputPair::Analog12,
                                Model::OutputSource::Mixer);
    assert(model.srcAnalogOut() == 0x00000002u);
    assert(!model.isStraightAnalogPlaybackPreset());

    model.loadStraightAnalogPlaybackPreset();
    assert(model.isStraightAnalogPlaybackPreset());
    return 0;
}
