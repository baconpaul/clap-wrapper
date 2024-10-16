#include "process.h"
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include "parameter.h"
#include <algorithm>

using namespace Steinberg;

void ProcessAdapter::setupProcessing(size_t numInputs, size_t numOutputs, size_t numEventInputs, size_t numEventOutputs, Steinberg::Vst::ParameterContainer& params)
{
  parameters = &params;

  _processData.audio_inputs_count = numInputs;
  if (numInputs > 0)
  {
    _processData.audio_inputs = &_inputs;
  }

  _processData.audio_outputs_count = numOutputs;
  if (numOutputs > 0)
  {
    _processData.audio_outputs = &_outputs;
  }

  _processData.in_events = &_in_events;
  _processData.out_events = &_out_events;

  _processData.transport = &_transport;

  _in_events.ctx = this;
  _in_events.size = input_events_size;
  _in_events.get = input_events_get;

  _out_events.ctx = this;
  _out_events.try_push = output_events_try_push;

  _events.clear();
  _events.reserve(256);
  _eventindices.clear();
  _eventindices.reserve(_events.capacity());

  _out_events.ctx = this;

}

inline clap_beattime doubleToBeatTime(double t)
{
  return round(t * CLAP_BEATTIME_FACTOR);
}

inline clap_sectime doubleToSecTime(double t)
{
  return round(t * CLAP_SECTIME_FACTOR);
}


// this converts the ProcessContext data from VST to CLAP
void ProcessAdapter::process(Steinberg::Vst::ProcessData& data, const clap_plugin_t* plugin)
{
  // remember the ProcessData pointer during process
  _vstdata = &data;

  /// convert timing
  _transport.header = {
    sizeof(_transport),
    0,
    CLAP_CORE_EVENT_SPACE_ID,
    CLAP_EVENT_TRANSPORT,
    0
  };

  _transport.flags = 0;
  if (_vstdata->processContext)
  {
    // converting the flags
    _transport.flags |= 0
      // kPlaying = 1 << 1,		///< currently playing
      | ((_vstdata->processContext->state & Vst::ProcessContext::kPlaying) ? CLAP_TRANSPORT_IS_PLAYING : 0)
      // kRecording = 1 << 3,		///< currently recording
      | ((_vstdata->processContext->state & Vst::ProcessContext::kRecording) ? CLAP_TRANSPORT_IS_RECORDING : 0)
      // kCycleActive = 1 << 2,		///< cycle is active
      | ((_vstdata->processContext->state & Vst::ProcessContext::kCycleActive) ? CLAP_TRANSPORT_IS_LOOP_ACTIVE : 0)
      // kTempoValid = 1 << 10,	///< tempo contains valid information
      | ((_vstdata->processContext->state & Vst::ProcessContext::kTempoValid) ? CLAP_TRANSPORT_HAS_TEMPO : 0)
      | ((_vstdata->processContext->state & Vst::ProcessContext::kBarPositionValid) ? CLAP_TRANSPORT_HAS_BEATS_TIMELINE : 0)
      | ((_vstdata->processContext->state & Vst::ProcessContext::kTimeSigValid) ? CLAP_TRANSPORT_HAS_TIME_SIGNATURE : 0)

      // the rest of the flags has no meaning to CLAP
      // kSystemTimeValid = 1 << 8,		///< systemTime contains valid information
      // kContTimeValid = 1 << 17,	///< continousTimeSamples contains valid information
      // 
      // kProjectTimeMusicValid = 1 << 9,///< projectTimeMusic contains valid information
      // kBarPositionValid = 1 << 11,	///< barPositionMusic contains valid information
      // kCycleValid = 1 << 12,	///< cycleStartMusic and barPositionMusic contain valid information
      // 
      // kClockValid = 1 << 15		///< samplesToNextClock valid
      // kTimeSigValid = 1 << 13,	///< timeSigNumerator and timeSigDenominator contain valid information
      // kChordValid = 1 << 18,	///< chord contains valid information
      // 
      // kSmpteValid = 1 << 14,	///< smpteOffset and frameRate contain valid information

      ;

    _transport.song_pos_beats = doubleToBeatTime(_vstdata->processContext->projectTimeMusic);
    _transport.song_pos_seconds = 0;

    _transport.tempo = _vstdata->processContext->tempo;
    _transport.tempo_inc = 0;

    _transport.loop_start_beats = doubleToBeatTime(_vstdata->processContext->cycleStartMusic);
    _transport.loop_end_beats = doubleToBeatTime(_vstdata->processContext->cycleEndMusic);
    _transport.loop_start_seconds = 0;
    _transport.loop_end_seconds = 0;

    _transport.bar_start = 0;
    _transport.bar_number = 0;

    if ((_vstdata->processContext->state & Vst::ProcessContext::kTimeSigValid))
    {
      _transport.tsig_num = _vstdata->processContext->timeSigNumerator;
      _transport.tsig_denom = _vstdata->processContext->timeSigDenominator;
    }
    else
    {
      _transport.tsig_num = 4;
      _transport.tsig_denom = 4;
    }

    _transport.bar_number = _vstdata->processContext->barPositionMusic;
    _processData.steady_time = _vstdata->processContext->projectTimeSamples;
  }

  // setting up transport
  _processData.frames_count = _vstdata->numSamples;

  clap_audio_buffer_t outs;

  // setting up buffers
  _processData.audio_inputs = nullptr;
  _processData.audio_inputs_count = 0;
  _processData.audio_outputs = &outs;
  _processData.audio_outputs_count = _vstdata->numOutputs;
  _processData.audio_outputs->channel_count = _vstdata->outputs->numChannels;
  _processData.audio_outputs->data32 = _vstdata->outputs->channelBuffers32;
  _processData.audio_outputs->constant_mask = 3;
  _processData.audio_outputs->latency = 0;

  _processData.audio_inputs_count = 0;

  // always clear
  _events.clear();
  _eventindices.clear();

  if (_vstdata->inputEvents)
  {
    Vst::Event vstevent;
    auto numev = _vstdata->inputEvents->getEventCount();
    for (decltype(numev) i = 0; i < numev; ++i)
    {
      if (_vstdata->inputEvents->getEvent(i, vstevent) == kResultOk)
      {
        if (vstevent.type == Vst::Event::kNoteOnEvent)
        {
          clap_multi_event_t n;
          n.note.header.type = CLAP_EVENT_NOTE_ON;
          n.note.header.flags = (vstevent.flags & Vst::Event::kIsLive) ? CLAP_EVENT_IS_LIVE : 0;
          n.note.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
          n.note.header.time = vstevent.sampleOffset;
          n.note.header.size = sizeof(clap_event_note);
          n.note.channel = vstevent.noteOn.channel;
          n.note.note_id = vstevent.noteOn.noteId;
          n.note.port_index = 0;
          n.note.velocity = vstevent.noteOn.velocity;
          n.note.key = vstevent.noteOn.pitch;
          _eventindices.push_back(_events.size());
          _events.push_back(n);
        }
        if (vstevent.type == Vst::Event::kNoteOffEvent)
        {
          clap_multi_event_t n;
          n.note.header.type = CLAP_EVENT_NOTE_OFF;
          n.note.header.flags = (vstevent.flags & Vst::Event::kIsLive) ? CLAP_EVENT_IS_LIVE : 0;
          n.note.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
          n.note.header.time = vstevent.sampleOffset;
          n.note.header.size = sizeof(clap_event_note);
          n.note.channel = vstevent.noteOn.channel;
          n.note.note_id = vstevent.noteOn.noteId;
          n.note.port_index = 0;
          n.note.velocity = vstevent.noteOn.velocity;
          n.note.key = vstevent.noteOn.pitch;
          _eventindices.push_back(_events.size());
          _events.push_back(n);
        }
        if (vstevent.type == Vst::Event::kDataEvent)
        {
          clap_multi_event_t n;
          if (vstevent.data.type == Vst::DataEvent::DataTypes::kMidiSysEx)
          {
            n.sysex.buffer = vstevent.data.bytes;
            n.sysex.size = vstevent.data.size;
            n.sysex.port_index = 0;
            n.sysex.header.type = CLAP_EVENT_MIDI_SYSEX;
            n.sysex.header.flags = vstevent.flags & Vst::Event::kIsLive ? CLAP_EVENT_IS_LIVE : 0;
            n.sysex.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            n.sysex.header.time = vstevent.sampleOffset;
            n.sysex.header.size = sizeof(n.sysex);
            _eventindices.push_back(_events.size());
            _events.push_back(n);
          }
          else
          {
            // there are no other event types yet
          }
        }
      }
    }
  }

  if (_vstdata->inputParameterChanges)
  {
    auto numPevent = _vstdata->inputParameterChanges->getParameterCount();
    for (decltype(numPevent) i = 0; i < numPevent; ++i)
    {
      auto k = _vstdata->inputParameterChanges->getParameterData(i);

      // get the Vst3Parameter
      auto param = (Vst3Parameter*)parameters->getParameter(k->getParameterId());
      auto nums = k->getPointCount();

      Vst::ParamValue value;
      int32 offset;
      if (k->getPoint(nums - 1, offset, value) == kResultOk)
      {
        clap_multi_event_t n;
        n.param.header.type = CLAP_EVENT_PARAM_VALUE;
        n.param.header.flags = 0;
        n.param.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        n.param.header.time = offset;
        n.param.header.size = sizeof(clap_event_param_value);
        n.param.param_id = param->id;
        n.param.cookie = param->cookie;

        // nothing note specific
        n.param.note_id = -1;   // always global
        n.param.port_index = -1;
        n.param.channel = -1;
        n.param.key = -1;

        n.param.value = param->asClapValue(value);
        _eventindices.push_back(_events.size());
        _events.push_back(n);
      }

    }
  }

  // just sorting the index
  std::sort(_eventindices.begin(), _eventindices.end(), [&](size_t const& a, size_t const& b)
    {
      return _events[a].header.time < _events[b].header.time;
    }
  );
  plugin->process(plugin, &_processData);
  
  _vstdata = nullptr;
}

uint32_t ProcessAdapter::input_events_size(const struct clap_input_events* list)
{
  auto self = static_cast<ProcessAdapter*>(list->ctx);
  return self->_events.size();
  // return self->_vstdata->inputEvents->getEventCount();
}

// returns the pointer to an event in the list. The index accessed is not the position in the event list itself
// since all events indices were sorted by timestamp
const clap_event_header_t* ProcessAdapter::input_events_get(const struct clap_input_events* list, uint32_t index)
{
  auto self = static_cast<ProcessAdapter*>(list->ctx);
  if ( self->_events.size() > index)
  {
    // we can safely return the note.header also for other event types
    // since they are at the same memory address
    auto realindex = self->_eventindices[index];
    return &(self->_events[realindex].header);
  }
  return nullptr;
}

bool ProcessAdapter::output_events_try_push(const struct clap_output_events* list, const clap_event_header_t* event)
{
  auto self = static_cast<ProcessAdapter*>(list->ctx);
  // mainly used for CLAP_EVENT_NOTE_CHOKE and CLAP_EVENT_NOTE_END
  // but also for parameter changes
  return true;
}
