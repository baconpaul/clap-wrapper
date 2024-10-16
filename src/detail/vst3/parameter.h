#pragma once

/*
    Vst3Parameter

    is being derived from Steinberg::Vst::Parameter and additionally stores
    information about the CLAP info (like the unique id of the CLAP parameter
    as well as the cookie pointer which is needed to address a parameter change
    properly.

    Still, the wrapper will use the ParameterContainer (std::vector<IPtr<Parameter>>)
    to communicate with the VST3 host.

    call Vst3Parameter::create(clapinfo) to create a heap based instance of it.

    The create function will apply everything necessary to the Vst::Parameter object.
*/

#include <clap/ext/params.h>
#include <public.sdk/source/vst/vstparameters.h>

class Vst3Parameter : public Steinberg::Vst::Parameter
{
protected:
  Vst3Parameter(const Steinberg::Vst::ParameterInfo& vst3info, const clap_param_info_t* clapinfo);
public:
  virtual ~Vst3Parameter();
  inline double asClapValue(double vst3value) const
  {
    return vst3value * (max_value - min_value) + min_value;
  }
  inline double asVst3Value(double clapvalue) const
  {
    return (clapvalue - min_value) / (max_value - min_value);
  }
  static Vst3Parameter* create(const clap_param_info_t* info);
  // copies from the clap_param_info_t
  clap_id id;
  void* cookie;
  double min_value;     // minimum plain value
  double max_value;     // maximum plain value

};
