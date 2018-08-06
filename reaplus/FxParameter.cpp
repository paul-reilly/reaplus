#include "FxParameter.h"
#include <memory>
#include <utility>
#include "ModelUtil.h"
#include "HelperControlSurface.h"
#include "utility.h"

#include "reaper_plugin_functions.h"

using std::string;
using std::unique_ptr;

namespace reaplus {
  FxParameter::FxParameter(Fx fx, int index) : fx_(std::move(fx)), index_(index) {
  }

  int FxParameter::index() const {
    return index_;
  }

  string FxParameter::name() const {
    return reaplus::toString(256, [this](char* buffer, int maxSize) {
      reaper::TrackFX_GetParamName(fx_.track().mediaTrack(), fx_.queryIndex(), index_, buffer, maxSize);
    });
  }

  Fx FxParameter::fx() const {
    return fx_;
  }

  string FxParameter::formattedValue() const {
    return reaplus::toString(256, [this](char* buffer, int maxSize) {
      reaper::TrackFX_GetFormattedParamValue(fx_.track().mediaTrack(), fx_.queryIndex(), index_, buffer, maxSize);
    });
  }

  double FxParameter::normalizedValue() const {
    // TODO deal with nullptr MediaTrack (empty string)
    return reaperValue();
  }

  std::string FxParameter::formatNormalizedValue(double normalizedValue) const {
    return reaplus::toString(256, [this, normalizedValue](char* buffer, int maxSize) {
      reaper::TrackFX_FormatParamValueNormalized(fx_.track().mediaTrack(), fx_.queryIndex(), index_, normalizedValue,
          buffer, maxSize);
    });
  }

  void FxParameter::setNormalizedValue(double normalizedValue) {
    // TODO deal with nullptr MediaTrack (do nothing)
    reaper::TrackFX_SetParamNormalized(fx().track().mediaTrack(), fx().queryIndex(), index(), normalizedValue);
  }

  FxParameterCharacter FxParameter::character() const {
    bool isToggle;
    double smallStepSize = -1;
    // TODO deal with nullptr MediaTrack (continuous)
    bool wasSuccessful = reaper::TrackFX_GetParameterStepSizes(fx().track().mediaTrack(), fx().queryIndex(), index(),
        nullptr, &smallStepSize, nullptr, &isToggle);
    if (wasSuccessful) {
      if (isToggle) {
        return FxParameterCharacter::Toggle;
      } else if (smallStepSize == -1) {
        return FxParameterCharacter::Continuous;
      } else {
        return FxParameterCharacter::Discrete;
      }
    } else {
      return FxParameterCharacter::Continuous;
    }

  }
  FxParameterValueRange FxParameter::valueRange() const {
    FxParameterValueRange range;
    reaper::TrackFX_GetParamEx(fx().track().mediaTrack(), fx().queryIndex(), index(),
        &range.minVal, &range.maxVal, &range.midVal);
    return range;
  }

  double FxParameter::stepSize() const {
    const auto range = valueRange();
    bool isToggle = false;
    double stepSize = -1;
    double smallStepSize = -1;
    double largeStepSize = -1;
    // TODO deal with nullptr MediaTrack
    bool wasSuccessful = reaper::TrackFX_GetParameterStepSizes(fx().track().mediaTrack(), fx().queryIndex(), index(),
        &stepSize, &smallStepSize, &largeStepSize, &isToggle);
    if (!wasSuccessful) {
      return -1;
    }
    if (isToggle) {
      return 1;
    }
    // We are primarily interested in the smallest step size that makes sense. We can always create multiples of it.
    if (smallStepSize != -1) {
      return ModelUtil::mapValueInRangeToNormalizedValue(smallStepSize, range.minVal, range.maxVal);
    }
    if (stepSize != -1) {
      return ModelUtil::mapValueInRangeToNormalizedValue(stepSize, range.minVal, range.maxVal);
    }
    if (largeStepSize != -1) {
      return ModelUtil::mapValueInRangeToNormalizedValue(largeStepSize, range.minVal, range.maxVal);
    }
  }

  bool operator==(const FxParameter& lhs, const FxParameter& rhs) {
    return lhs.fx_ == rhs.fx_ && lhs.index_ == rhs.index_;
  }

  bool operator!=(const FxParameter& lhs, const FxParameter& rhs) {
    return !(lhs == rhs);
  }

  double FxParameter::reaperValue() const {
    return reaper::TrackFX_GetParamNormalized(fx_.track().mediaTrack(), fx_.queryIndex(), index_);
  }

  ParameterType FxParameter::parameterType() const {
    return ParameterType::FX;
  }

  bool FxParameter::equals(const Parameter& other) const {
    auto& o = dynamic_cast<const FxParameter&>(other);
    // TODO Do we still need the specialized == operator implementation if we already have the polymorphic one?
    return *this == o;
  }

  Track FxParameter::track() const {
    return fx_.track();
  }

  unique_ptr<Parameter> FxParameter::clone() const {
    return std::make_unique<FxParameter>(*this);
  }

  bool FxParameter::isAvailable() const {
    return fx_.isAvailable() && index_ < fx_.parameterCount();
  }

}