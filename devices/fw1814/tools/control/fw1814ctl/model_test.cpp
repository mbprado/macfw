#include "../../../special_mixer_model.h"

#include <cassert>

int main() {
    using Model = macfw::fw1814::SpecialMixerRoutingModel;

    Model model;
    model.loadStraightAnalogPlaybackPreset();
    assert(model.mixStreamIn() == 0x00000006u);
    assert(model.mixAnalogDigitalIn() == 0x00000000u);
    assert(model.srcHeadphoneOut() == 0x00010001u);
    assert(model.srcAnalogOut() == 0x00000000u);
    assert(model.isStraightAnalogPlaybackPreset());

    assert(macfw::fw1814::stereoMonitorLevelWord(
               macfw::fw1814::kMonitorLevelMute) == 0x80008000u);
    assert(macfw::fw1814::stereoMonitorLevelWord(
               macfw::fw1814::kMonitorLevelUnity) == 0x00000000u);
    assert(macfw::fw1814::stereoMonitorLevelWord(
               macfw::fw1814::kMonitorLevelMinus20Db) == 0xec00ec00u);
    assert(macfw::fw1814::setMonitorLevelChannel(
               0x00000000u, 0,
               macfw::fw1814::kMonitorLevelMinus20Db) == 0xec000000u);
    assert(macfw::fw1814::setMonitorLevelChannel(
               0xec000000u, 1,
               macfw::fw1814::kMonitorLevelMinus20Db) == 0xec00ec00u);
    assert(macfw::fw1814::monitorLevelChannel(0xec000000u, 0) ==
           macfw::fw1814::kMonitorLevelMinus20Db);
    assert(macfw::fw1814::monitorLevelChannel(0xec000000u, 1) ==
           macfw::fw1814::kMonitorLevelUnity);
    assert(macfw::fw1814::monitorLevelFromDb(-128) ==
           macfw::fw1814::kMonitorLevelMute);
    assert(macfw::fw1814::monitorLevelFromDb(-20) ==
           macfw::fw1814::kMonitorLevelMinus20Db);
    assert(macfw::fw1814::monitorLevelFromDb(0) ==
           macfw::fw1814::kMonitorLevelUnity);
    assert(macfw::fw1814::monitorLevelRaw(
               macfw::fw1814::kMonitorLevelMute) == -32768);
    assert(macfw::fw1814::monitorLevelRaw(
               macfw::fw1814::kMonitorLevelMinus20Db) == -5120);
    assert(macfw::fw1814::monitorLevelDb(
               macfw::fw1814::monitorLevelFromDb(-73)) == -73);
    assert(macfw::fw1814::inputPanChannel(
               macfw::fw1814::kAnalogInputPanBaseline, 0) ==
           macfw::fw1814::kPanHardLeft);
    assert(macfw::fw1814::inputPanChannel(
               macfw::fw1814::kAnalogInputPanBaseline, 1) ==
           macfw::fw1814::kPanHardRight);
    assert(macfw::fw1814::setInputPanChannel(
               macfw::fw1814::kAnalogInputPanBaseline, 0,
               macfw::fw1814::kPanCenter) == 0x00008000u);
    assert(macfw::fw1814::setInputPanChannel(
               macfw::fw1814::kAnalogInputPanBaseline, 1,
               macfw::fw1814::kPanCenter) == 0x7ffe0000u);
    assert(macfw::fw1814::setInputPanChannel(
               0x00008000u, 0,
               macfw::fw1814::kPanHardRight) == 0x80008000u);
    assert(macfw::fw1814::setInputPanChannel(
               0x80008000u, 1,
               macfw::fw1814::kPanCenter) == 0x80000000u);
    assert(macfw::fw1814::setInputPanChannel(
               0x00008000u, 1,
               macfw::fw1814::kPanCenter) == 0x00000000u);
    assert(macfw::fw1814::setInputPanChannel(
               macfw::fw1814::kAnalogInputPanBaseline, 0,
               macfw::fw1814::kPanHalfLeft) == 0x40008000u);
    assert(macfw::fw1814::setInputPanChannel(
               macfw::fw1814::kAnalogInputPanBaseline, 1,
               macfw::fw1814::kPanHalfRight) == 0x7ffec000u);
    assert(macfw::fw1814::inputPanFromPercent(-100) ==
           macfw::fw1814::kPanHardLeft);
    assert(macfw::fw1814::inputPanFromPercent(-50) ==
           macfw::fw1814::kPanHalfLeft);
    assert(macfw::fw1814::inputPanFromPercent(0) ==
           macfw::fw1814::kPanCenter);
    assert(macfw::fw1814::inputPanFromPercent(50) ==
           macfw::fw1814::kPanHalfRight);
    assert(macfw::fw1814::inputPanFromPercent(100) ==
           macfw::fw1814::kPanHardRight);
    for (int percent = -100; percent <= 100; ++percent)
        assert(macfw::fw1814::inputPanPercent(
                   macfw::fw1814::inputPanFromPercent(percent)) == percent);

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
    assert(!model.analogInputRoute(Model::AnalogInputPair::Analog12,
                                   Model::MixerBus::Mixer12));
    model.setAnalogInputRoute(Model::AnalogInputPair::Analog12,
                              Model::MixerBus::Mixer12, true);
    assert(model.mixAnalogDigitalIn() == 0x00000001u);
    assert(model.analogInputRoute(Model::AnalogInputPair::Analog12,
                                  Model::MixerBus::Mixer12));
    model.setAnalogInputRoute(Model::AnalogInputPair::Analog12,
                              Model::MixerBus::Mixer34, true);
    assert(model.mixAnalogDigitalIn() == 0x00000011u);
    assert(!model.isStraightAnalogPlaybackPreset());
    model.loadStraightAnalogPlaybackPreset();
    assert(model.mixAnalogDigitalIn() == 0x00000000u);

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
