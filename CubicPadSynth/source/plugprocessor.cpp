// Original by:
// (c) 2018, Steinberg Media Technologies GmbH, All Rights Reserved
//
// Modified by:
// (c) 2020 Takamitsu Endo
//
// This file is part of CubicPadSynth.
//
// CubicPadSynth is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// CubicPadSynth is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with CubicPadSynth.  If not, see <https://www.gnu.org/licenses/>.

#include "plugprocessor.hpp"
#include "fuid.hpp"

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include "../../lib/vcl/vectorclass.h"

#include <cstring>
#include <iostream>

namespace Steinberg {
namespace Synth {

PlugProcessor::PlugProcessor()
{
  auto iset = instrset_detect();
  if (iset >= 10) {
    dsp = std::make_unique<DSPCore_AVX512>();
  } else if (iset >= 8) {
    dsp = std::make_unique<DSPCore_AVX2>();
  } else if (iset >= 7) {
    dsp = std::make_unique<DSPCore_AVX>();
  } else {
    std::cerr << "\nError: Instruction set AVX or later not supported on this computer";
    exit(EXIT_FAILURE);
  }

  setControllerClass(ControllerUID);
}

tresult PLUGIN_API PlugProcessor::initialize(FUnknown *context)
{
  tresult result = AudioEffect::initialize(context);
  if (result != kResultTrue) return result;

  addAudioOutput(STR16("StereoOutput"), Vst::SpeakerArr::kStereo);
  addEventInput(STR16("EventInput"), 1);

  return result;
}

tresult PLUGIN_API PlugProcessor::setBusArrangements(
  Vst::SpeakerArrangement *inputs,
  int32 numIns,
  Vst::SpeakerArrangement *outputs,
  int32 numOuts)
{
  if (numIns == 1 && numOuts == 1 && inputs[0] == outputs[0]) {
    return AudioEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
  }
  return kResultFalse;
}

tresult PLUGIN_API PlugProcessor::setupProcessing(Vst::ProcessSetup &setup)
{
  bypassFadeLength = int64_t(0.04 * setup.sampleRate); // 0.04 seconds fade-out.
  bypassCounter = bypassFadeLength;
  return AudioEffect::setupProcessing(setup);
}

tresult PLUGIN_API PlugProcessor::setActive(TBool state)
{
  if (state) {
    dsp->setup(processSetup.sampleRate);
  } else {
    lastState = 0;
  }
  return AudioEffect::setActive(state);
}

tresult PLUGIN_API PlugProcessor::process(Vst::ProcessData &data)
{
  // Read inputs parameter changes.
  if (data.inputParameterChanges) {
    int32 parameterCount = data.inputParameterChanges->getParameterCount();
    for (int32 index = 0; index < parameterCount; index++) {
      auto queue = data.inputParameterChanges->getParameterData(index);
      if (!queue) continue;
      Vst::ParamValue value;
      int32 sampleOffset;
      if (queue->getPoint(queue->getPointCount() - 1, sampleOffset, value) != kResultTrue)
        continue;
      size_t id = queue->getParameterId();
      if (id < dsp->param.value.size()) dsp->param.value[id]->setFromNormalized(value);
    }
  }

  if (data.processContext != nullptr) {
    uint64_t state = data.processContext->state;
    if (
      (lastState & Vst::ProcessContext::kPlaying) == 0
      && (state & Vst::ProcessContext::kPlaying) != 0) {
      dsp->startup();
    }
    lastState = state;
    tempo = data.processContext->tempo;
  }
  dsp->setParameters(tempo);

  if (data.numOutputs == 0) return kResultOk;
  if (data.numSamples <= 0) return kResultOk;
  if (data.outputs[0].numChannels != 2) return kResultOk;
  if (data.symbolicSampleSize == Vst::kSample64) return kResultOk;

  if (data.inputEvents != nullptr) handleEvent(data);
  processSignal(data);

  return kResultOk;
}

void PlugProcessor::processSignal(Vst::ProcessData &data)
{
  float *out0 = data.outputs[0].channelBuffers32[0];
  float *out1 = data.outputs[0].channelBuffers32[1];
  size_t length = (size_t)data.numSamples;
  if (dsp->param.value[ParameterID::bypass]->getInt()) {
    if (bypassCounter > 0) { // Fade-out.
      dsp->process(length, out0, out1);
      for (size_t i = 0; i < length; ++i) {
        bypassCounter -= 1;
        if (bypassCounter < 0) bypassCounter = 0;
        const auto gain = float(bypassCounter) / bypassFadeLength;
        out0[i] *= gain;
        out1[i] *= gain;
      }
    } else { // Bypass.
      memset(out0, 0.0f, length * sizeof(float));
      memset(out1, 0.0f, length * sizeof(float));
    }
  } else {
    if (bypassCounter < bypassFadeLength) { // Fade-in.
      dsp->process(length, out0, out1);
      for (size_t i = 0; i < length; ++i) {
        bypassCounter += 1;
        if (bypassCounter > bypassFadeLength) bypassCounter = bypassFadeLength;
        const auto gain = float(bypassCounter) / bypassFadeLength;
        out0[i] *= gain;
        out1[i] *= gain;
      }
    } else { // Main processing.
      dsp->process(length, out0, out1);
    }
  }
}

void PlugProcessor::handleEvent(Vst::ProcessData &data)
{
  for (int32 index = 0; index < data.inputEvents->getEventCount(); ++index) {
    Vst::Event event;
    if (data.inputEvents->getEvent(index, event) != kResultOk) continue;
    switch (event.type) {
      case Vst::Event::kNoteOnEvent: {
        // List of DAW that don't support note ID. Probably more.
        // - Ableton Live 10.1.6
        // - PreSonus Studio One 4.6.1
        auto noteId
          = event.noteOn.noteId == -1 ? event.noteOn.pitch : event.noteOn.noteId;
        dsp->pushMidiNote(
          true, event.sampleOffset, noteId, event.noteOn.pitch, event.noteOn.tuning,
          event.noteOn.velocity);
      } break;

      case Vst::Event::kNoteOffEvent: {
        auto noteId
          = event.noteOff.noteId == -1 ? event.noteOff.pitch : event.noteOff.noteId;
        dsp->pushMidiNote(false, event.sampleOffset, noteId, 0, 0, 0);
      } break;

        // Add other event type here.
    }
  }
}

tresult PLUGIN_API PlugProcessor::setState(IBStream *state)
{
  if (!state) return kResultFalse;
  return dsp->param.setState(state);
}

tresult PLUGIN_API PlugProcessor::getState(IBStream *state)
{
  return dsp->param.getState(state);
}

tresult PlugProcessor::receiveText(const char8 *text)
{
  if (std::strcmp(text, "padsynth") == 0)
    dsp->refreshTable();
  else if (std::strcmp(text, "lfo") == 0)
    dsp->refreshLfo();
  return kResultOk;
}

} // namespace Synth
} // namespace Steinberg
