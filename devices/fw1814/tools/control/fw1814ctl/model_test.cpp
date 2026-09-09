#include "../../../special_mixer_model.h"

#include <cassert>

int main() {
    using Model = macfw::fw1814::SpecialMixerRoutingModel;

    Model model;
    model.loadStraightAnalogPlaybackPreset();
    assert(model.mixStreamIn() == 0x00000006u);
    assert(model.srcHeadphoneOut() == 0x00010001u);
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

    assert(Model::kAnalogInputRouteMasks[0][0] == 0x01u);
    assert(Model::kAnalogInputRouteMasks[0][1] == 0x10u);
    assert(Model::kAnalogInputRouteMasks[1][0] == 0x02u);
    assert(Model::kAnalogInputRouteMasks[1][1] == 0x20u);
    assert(Model::kAnalogInputRouteMasks[2][0] == 0x04u);
    assert(Model::kAnalogInputRouteMasks[2][1] == 0x40u);
    assert(Model::kAnalogInputRouteMasks[3][0] == 0x08u);
    assert(Model::kAnalogInputRouteMasks[3][1] == 0x80u);

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

    using HeadphoneSource = macfw::fw1814::HeadphoneSource;
    assert(macfw::fw1814::headphoneSourceWord(
               HeadphoneSource::Mixer12, HeadphoneSource::Mixer12) ==
           0x00010001u);
    assert(macfw::fw1814::headphoneSourceWord(
               HeadphoneSource::Mixer34, HeadphoneSource::Aux12) ==
           0x00040002u);

    assert(model.headphoneSource(Model::HeadphoneOutput::Output1) ==
           HeadphoneSource::Mixer12);
    assert(model.headphoneSource(Model::HeadphoneOutput::Output2) ==
           HeadphoneSource::Mixer12);
    model.setHeadphoneSource(Model::HeadphoneOutput::Output2,
                             HeadphoneSource::Mixer34);
    assert(model.srcHeadphoneOut() == 0x00020001u);
    assert(model.headphoneSource(Model::HeadphoneOutput::Output2) ==
           HeadphoneSource::Mixer34);
    assert(!model.isStraightAnalogPlaybackPreset());
    model.setHeadphoneSource(Model::HeadphoneOutput::Output1,
                             HeadphoneSource::Aux12);
    assert(model.srcHeadphoneOut() == 0x00020004u);
    model.loadStraightAnalogPlaybackPreset();
    assert(model.isStraightAnalogPlaybackPreset());
    return 0;
}
