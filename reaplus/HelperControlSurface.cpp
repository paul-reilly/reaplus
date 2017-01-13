#include "HelperControlSurface.h"

#include "TrackSend.h"
#include "Reaper.h"
#include "TrackVolume.h"
#include "TrackPan.h"
#include "TrackSendVolume.h"
#include "reaper_plugin_functions.h"

using std::unique_lock;
namespace rx = rxcpp;
namespace rxsub = rxcpp::subjects;
using std::function;
using std::mutex;
using std::pair;
using std::string;
using std::set;


namespace reaplus {
  HelperControlSurface::HelperControlSurface() : activeProjectBehavior_(Reaper::instance().currentProject()) {
    reaper::plugin_register("csurf_inst", this);
  }

  rxcpp::subscription HelperControlSurface::enqueueCommand(function<void(void)> command) {
    rxcpp::composite_subscription subscription;
    const auto& worker = mainThreadCoordinator_.get_worker();
    worker.schedule(
        rxcpp::schedulers::make_schedulable(worker, subscription, [command](const rxcpp::schedulers::schedulable&) {
          command();
        })
    );
    return subscription;
  }

  const char* HelperControlSurface::GetTypeString() {
    return "";
  }

  const char* HelperControlSurface::GetDescString() {
    return "";
  }

  const char* HelperControlSurface::GetConfigString() {
    return "";
  }

  void HelperControlSurface::Run() {
    while (!mainThreadRunLoop_.empty() && mainThreadRunLoop_.peek().when <= mainThreadRunLoop_.now()) {
      mainThreadRunLoop_.dispatch();
    }
  }


  void HelperControlSurface::SetSurfaceVolume(MediaTrack* trackid, double volume) {
    if (state() != State::PropagatingTrackSetChanges) {
      if (auto td = findTrackDataByTrack(trackid)) {
        if (td->volume != volume) {
          td->volume = volume;
          Track track(trackid, nullptr);
          trackVolumeChangedSubject_.get_subscriber().on_next(track);
          if (!trackParameterIsAutomated(track, "Volume")) {
            trackVolumeTouchedSubject_.get_subscriber().on_next(track);
          }
        }
      }
    }
  }


  void HelperControlSurface::SetSurfacePan(MediaTrack* trackid, double pan) {
    if (state() != State::PropagatingTrackSetChanges) {
      if (auto td = findTrackDataByTrack(trackid)) {
        if (td->pan != pan) {
          td->pan = pan;
          Track track(trackid, nullptr);
          trackPanChangedSubject_.get_subscriber().on_next(track);
          if (!trackParameterIsAutomated(track, "Pan")) {
            trackPanTouchedSubject_.get_subscriber().on_next(track);
          }
        }
      }
    }
  }


  HelperControlSurface& HelperControlSurface::instance() {
    static HelperControlSurface INSTANCE;
    return INSTANCE;
  }

  rx::observable <FxParameter> HelperControlSurface::fxParameterValueChanged() const {
    return fxParameterValueChangedSubject_.get_observable();
  }

  void HelperControlSurface::SetTrackTitle(MediaTrack* trackid, const char* title) {
    if (state() == State::PropagatingTrackSetChanges) {
      numTrackSetChangesLeftToBePropagated_--;
    } else {
      trackNameChangedSubject_.get_subscriber().on_next(Track(trackid, nullptr));
    }
  }

  rxcpp::observable<Fx> HelperControlSurface::fxEnabledChanged() const {
    return fxEnabledChangedSubject_.get_observable();
  }

  int HelperControlSurface::Extended(int call, void* parm1, void* parm2, void* parm3) {
    switch (call) {
    case CSURF_EXT_SETINPUTMONITOR: {
      if (state() != State::PropagatingTrackSetChanges) {
        const auto mediaTrack = (MediaTrack*) parm1;
        if (auto td = findTrackDataByTrack(mediaTrack)) {
          {
            const auto recmonitor = (int*) parm2;
            if (td->recmonitor != *recmonitor) {
              td->recmonitor = *recmonitor;
              trackInputMonitoringChangedSubject_.get_subscriber().on_next(Track(mediaTrack, nullptr));
            }
          }
          {
            const int recinput = (int) reaper::GetMediaTrackInfo_Value(mediaTrack, "I_RECINPUT");
            if (td->recinput != recinput) {
              td->recinput = recinput;
              trackInputChangedSubject_.get_subscriber().on_next(Track(mediaTrack, nullptr));
            }
          }
        }
      }
      return 0;
    }
    case CSURF_EXT_SETFXPARAM: {
      const auto mediaTrack = (MediaTrack*) parm1;
      const auto fxAndParamIndex = (int*) parm2;
      const int fxIndex = (*fxAndParamIndex >> 16) & 0xffff;
      const int paramIndex = *fxAndParamIndex & 0xffff;
      // Unfortunately, we don't have a ReaProject* here. Therefore we pass a nullptr.
      const Track track(mediaTrack, nullptr);
      const double paramValue = *(double*) parm3;
      // TODO In the ReaPlus integration test, there's one test where this heuristic doesn't work. Deal with it!
      const bool isInputFx = isProbablyInputFx(track, fxIndex, paramIndex, paramValue);
      const auto fxChain = isInputFx ? track.inputFxChain() : track.normalFxChain();
      if (const auto fx = fxChain.fxByIndex(fxIndex)) {
        const auto fxParam = fx->parameterByIndex(paramIndex);
        fxParameterValueChangedSubject_.get_subscriber().on_next(fxParam);
        if (fxHasBeenTouchedJustAMomentAgo_) {
          fxHasBeenTouchedJustAMomentAgo_ = false;
          fxParameterTouchedSubject_.get_subscriber().on_next(fxParam);
        }
      }
      return 0;
    }
    case CSURF_EXT_SETFXENABLED: {
      const auto mediaTrack = (MediaTrack*) parm1;
      const auto fxIndex = *(int*) parm2;
      // Unfortunately, we don't have a ReaProject* here. Therefore we pass a nullptr.
      const Track track(mediaTrack, nullptr);
      const bool isInputFx = isProbablyInputFx(track, fxIndex);
      const auto fxChain = isInputFx ? track.inputFxChain() : track.normalFxChain();
      if (const auto fx = fxChain.fxByIndex(fxIndex)) {
        fxEnabledChangedSubject_.get_subscriber().on_next(*fx);
      }
      return 0;
    }
    case CSURF_EXT_SETSENDVOLUME: {
      const auto mediaTrack = (MediaTrack*) parm1;
      const int sendIdx = *(int*) parm2;
      const Track track(mediaTrack, nullptr);
      const auto trackSend = track.indexBasedSendByIndex(sendIdx);
      trackSendVolumeChangedSubject_.get_subscriber().on_next(trackSend);

      // Send volume touch event only if not automated
      if (!trackParameterIsAutomated(track, "Send Volume")) {
        trackSendVolumeTouchedSubject_.get_subscriber().on_next(trackSend);
      }
      return 0;
    }
    case CSURF_EXT_SETFXCHANGE: {
      const auto mediaTrack = (MediaTrack*) parm1;
      detectFxChangesOnTrack(Track(mediaTrack, nullptr), true);
      return 0;
    }
    case CSURF_EXT_SETLASTTOUCHEDFX: {
      fxHasBeenTouchedJustAMomentAgo_ = true;
      return 0;
    }
    default:
      return 0;
    }

  }

  rxcpp::observable<Track> HelperControlSurface::trackInputMonitoringChanged() const {
    return trackInputMonitoringChangedSubject_.get_observable();
  }

  rxcpp::observable<Track> HelperControlSurface::trackArmChanged() const {
    return trackArmChangedSubject_.get_observable();
  }

  rxcpp::observable<Track> HelperControlSurface::trackMuteChanged() const {
    return trackMuteChangedSubject_.get_observable();
  }

  rxcpp::observable<Track> HelperControlSurface::trackSoloChanged() const {
    return trackSoloChangedSubject_.get_observable();
  }

  rxcpp::observable<Track> HelperControlSurface::trackSelectedChanged() const {
    return trackSelectedChangedSubject_.get_observable();
  }

  rxcpp::observable<Project> HelperControlSurface::projectSwitched() const {
    return activeProjectBehavior_.get_observable();
  }

  void HelperControlSurface::SetTrackListChange() {
    // FIXME Not multi-project compatible!
    const auto newActiveProject = Reaper::instance().currentProject();
    if (newActiveProject != activeProjectBehavior_.get_value()) {
      activeProjectBehavior_.get_subscriber().on_next(newActiveProject);
    }
    numTrackSetChangesLeftToBePropagated_ = reaper::CountTracks(nullptr) + 1;
    removeInvalidReaProjects();
    detectTrackSetChanges();
  }

  HelperControlSurface::State HelperControlSurface::state() const {
    return numTrackSetChangesLeftToBePropagated_ == 0 ? State::Normal : State::PropagatingTrackSetChanges;
  }

  void HelperControlSurface::detectTrackSetChanges() {
    const auto project = Reaper::instance().currentProject();
    auto& oldTrackDatas = trackDataByMediaTrackByReaProject_[project.reaProject()];
    const int oldTrackCount = (int) oldTrackDatas.size();
    const int newTrackCount = project.trackCount();
    if (newTrackCount < oldTrackCount) {
      removeInvalidMediaTracks(project, oldTrackDatas);
    } else if (newTrackCount > oldTrackCount) {
      addMissingMediaTracks(project, oldTrackDatas);
    } else {
      updateMediaTrackPositions(project, oldTrackDatas);
    }
  }

  void HelperControlSurface::addMissingMediaTracks(const Project& project, TrackDataMap& trackDatas) {
    project.tracks().subscribe([this, &trackDatas](Track track) {
      auto mediaTrack = track.mediaTrack();
      if (trackDatas.count(mediaTrack) == 0) {
        TrackData d;
        d.recarm = reaper::GetMediaTrackInfo_Value(mediaTrack, "I_RECARM") != 0;
        d.mute = reaper::GetMediaTrackInfo_Value(mediaTrack, "B_MUTE") != 0;
        d.number = (int) (size_t) reaper::GetSetMediaTrackInfo(mediaTrack, "IP_TRACKNUMBER", nullptr);
        d.pan = reaper::GetMediaTrackInfo_Value(mediaTrack, "D_PAN");
        d.volume = reaper::GetMediaTrackInfo_Value(mediaTrack, "D_VOL");
        d.selected = reaper::GetMediaTrackInfo_Value(mediaTrack, "I_SELECTED") != 0;
        d.solo = reaper::GetMediaTrackInfo_Value(mediaTrack, "I_SOLO") != 0;
        d.recmonitor = (int) reaper::GetMediaTrackInfo_Value(mediaTrack, "I_RECMON");
        d.recinput = (int) reaper::GetMediaTrackInfo_Value(mediaTrack, "I_RECINPUT");
        trackDatas[mediaTrack] = d;
        trackAddedSubject_.get_subscriber().on_next(track);
        detectFxChangesOnTrack(track, false);
      }
    });
  }

  void HelperControlSurface::updateMediaTrackPositions(const Project& project,
      HelperControlSurface::TrackDataMap& trackDatas) {
    bool tracksHaveBeenReordered = false;
    for (auto it = trackDatas.begin(); it != trackDatas.end();) {
      const auto mediaTrack = it->first;
      if (reaper::ValidatePtr2(project.reaProject(), (void*) mediaTrack, "MediaTrack*")) {
        auto& trackData = it->second;
        const int newNumber = (int) (size_t) reaper::GetSetMediaTrackInfo(mediaTrack, "IP_TRACKNUMBER", nullptr);
        if (newNumber != trackData.number) {
          tracksHaveBeenReordered = true;
          trackData.number = newNumber;
        }
      }
      it++;
    }
    if (tracksHaveBeenReordered) {
      tracksReorderedSubject_.get_subscriber().on_next(project);
    }
  }

  TrackData* HelperControlSurface::findTrackDataByTrack(MediaTrack* mediaTrack) {
    const auto project = Reaper::instance().currentProject();
    auto& trackDatas = trackDataByMediaTrackByReaProject_[project.reaProject()];
    if (trackDatas.count(mediaTrack) == 0) {
      return nullptr;
    } else {
      return &trackDatas.at(mediaTrack);
    }
  }

  void HelperControlSurface::SetSurfaceMute(MediaTrack* trackid, bool mute) {
    if (state() != State::PropagatingTrackSetChanges) {
      if (auto td = findTrackDataByTrack(trackid)) {
        if (td->mute != mute) {
          td->mute = mute;
          Track track(trackid, nullptr);
          trackMuteChangedSubject_.get_subscriber().on_next(track);
        }
      }
    }
  }

  void HelperControlSurface::SetSurfaceSelected(MediaTrack* trackid, bool selected) {
    if (state() != State::PropagatingTrackSetChanges) {
      if (auto td = findTrackDataByTrack(trackid)) {
        if (td->selected != selected) {
          td->selected = selected;
          Track track(trackid, nullptr);
          trackSelectedChangedSubject_.get_subscriber().on_next(track);
        }
      }
    }
  }

  void HelperControlSurface::SetSurfaceSolo(MediaTrack* trackid, bool solo) {
    if (state() != State::PropagatingTrackSetChanges) {
      if (auto td = findTrackDataByTrack(trackid)) {
        if (td->solo != solo) {
          td->solo = solo;
          Track track(trackid, nullptr);
          trackSoloChangedSubject_.get_subscriber().on_next(track);
        }
      }
    }
  }

  void HelperControlSurface::SetSurfaceRecArm(MediaTrack* trackid, bool recarm) {
    if (state() != State::PropagatingTrackSetChanges) {
      if (auto td = findTrackDataByTrack(trackid)) {
        if (td->recarm != recarm) {
          td->recarm = recarm;
          Track track(trackid, nullptr);
          trackArmChangedSubject_.get_subscriber().on_next(track);
        }
      }
    }
  }

  void HelperControlSurface::removeInvalidMediaTracks(const Project& project, TrackDataMap& trackDatas) {
    for (auto it = trackDatas.begin(); it != trackDatas.end();) {
      const auto mediaTrack = it->first;
      if (reaper::ValidatePtr2(project.reaProject(), (void*) mediaTrack, "MediaTrack*")) {
        it++;
      } else {
        fxChainPairByMediaTrack_.erase(mediaTrack);
        trackRemovedSubject_.get_subscriber().on_next(Track(mediaTrack, project.reaProject()));
        it = trackDatas.erase(it);
      }
    }
  }

  void HelperControlSurface::removeInvalidReaProjects() {
    for (auto it = trackDataByMediaTrackByReaProject_.begin(); it != trackDataByMediaTrackByReaProject_.end();) {
      const auto pair = *it;
      const auto project = pair.first;
      if (reaper::ValidatePtr2(nullptr, (void*) project, "ReaProject*")) {
        it++;
      } else {
        it = trackDataByMediaTrackByReaProject_.erase(it);
      }
    }
  }

  rx::observable <Track> HelperControlSurface::trackRemoved() const {
    return trackRemovedSubject_.get_observable();
  }

  rx::observable <Track> HelperControlSurface::trackAdded() const {
    return trackAddedSubject_.get_observable();
  }

  rx::observable <Track> HelperControlSurface::trackVolumeChanged() const {
    return trackVolumeChangedSubject_.get_observable();
  }

  rx::observable <Track> HelperControlSurface::trackPanChanged() const {
    return trackPanChangedSubject_.get_observable();
  }

  rxcpp::observable<Track> HelperControlSurface::trackNameChanged() const {
    return trackNameChangedSubject_.get_observable();
  }

  rxcpp::observable<Track> HelperControlSurface::trackInputChanged() const {
    return trackInputChangedSubject_.get_observable();
  }

  rx::observable <TrackSend> HelperControlSurface::trackSendVolumeChanged() const {
    return trackSendVolumeChangedSubject_.get_observable();
  }

  const rxcpp::observe_on_one_worker& HelperControlSurface::mainThreadCoordination() const {
    return mainThreadCoordination_;
  }

  void HelperControlSurface::init() {
    HelperControlSurface::instance();
  }

  rx::observable <Track> HelperControlSurface::fxReordered() const {
    return fxReorderedSubject_.get_observable();
  }

  void HelperControlSurface::detectFxChangesOnTrack(Track track, bool notifyListenersAboutChanges) {
    if (track.isAvailable()) {
      MediaTrack* mediaTrack = track.mediaTrack();
      auto& fxChainPair = fxChainPairByMediaTrack_[mediaTrack];
      const bool addedOrRemovedOutputFx = detectFxChangesOnTrack(track, fxChainPair.outputFxGuids, false, notifyListenersAboutChanges);
      const bool addedOrRemovedInputFx = detectFxChangesOnTrack(track, fxChainPair.inputFxGuids, true, notifyListenersAboutChanges);
      if (notifyListenersAboutChanges && !addedOrRemovedInputFx && !addedOrRemovedOutputFx) {
        fxReorderedSubject_.get_subscriber().on_next(track);
      }
    }
  }

  void HelperControlSurface::CloseNoReset() {
    bool dummy = false;
  }

  void HelperControlSurface::SetPlayState(bool play, bool pause, bool rec) {
    bool dummy = false;
  }

  void HelperControlSurface::SetRepeatState(bool rep) {
    bool dummy = false;
  }

  void HelperControlSurface::SetAutoMode(int mode) {
    bool dummy = false;
  }

  void HelperControlSurface::ResetCachedVolPanStates() {
    bool dummy = false;
  }

  void HelperControlSurface::OnTrackSelection(MediaTrack* trackid) {
    bool dummy = false;
  }

  bool HelperControlSurface::detectFxChangesOnTrack(Track track, set<string>& oldFxGuids, bool isInputFx, bool notifyListenersAboutChanges) {
    const int oldFxCount = (int) oldFxGuids.size();
    const int newFxCount = (isInputFx ? track.inputFxChain() : track.normalFxChain()).fxCount();
    if (newFxCount < oldFxCount) {
      removeInvalidFx(track, oldFxGuids, isInputFx, notifyListenersAboutChanges);
      return true;
    } else if (newFxCount > oldFxCount) {
      addMissingFx(track, oldFxGuids, isInputFx, notifyListenersAboutChanges);
      return true;
    } else {
      // Reordering (or nothing)
      return false;
    }
  }


  void HelperControlSurface::removeInvalidFx(Track track, std::set<string>& oldFxGuids, bool isInputFx, bool notifyListenersAboutChanges) {
    const auto newFxGuids = fxGuidsOnTrack(track, isInputFx);
    for (auto it = oldFxGuids.begin(); it != oldFxGuids.end();) {
      const string oldFxGuid = *it;
      if (newFxGuids.count(oldFxGuid)) {
        it++;
      } else {
        if (notifyListenersAboutChanges) {
          const auto fxChain = isInputFx ? track.inputFxChain() : track.normalFxChain();
          fxRemovedSubject_.get_subscriber().on_next(fxChain.fxByGuid(oldFxGuid));
        }
        it = oldFxGuids.erase(it);
      }
    }
  }

  void HelperControlSurface::addMissingFx(Track track, std::set<string>& fxGuids, bool isInputFx, bool notifyListenersAboutChanges) {
    const auto fxChain = isInputFx ? track.inputFxChain() : track.normalFxChain();
    fxChain.fxs().subscribe([this, &fxGuids, notifyListenersAboutChanges](Fx fx) {
      bool wasInserted = fxGuids.insert(fx.guid()).second;
      if (wasInserted && notifyListenersAboutChanges) {
        fxAddedSubject_.get_subscriber().on_next(fx);
      }
    });
  }

  std::set<string> HelperControlSurface::fxGuidsOnTrack(Track track, bool isInputFx) const {
    const auto fxChain = isInputFx ? track.inputFxChain() : track.normalFxChain();
    std::set<string> fxGuids;
    fxChain.fxs().subscribe([&fxGuids](Fx fx) {
      fxGuids.insert(fx.guid());
    });
    return fxGuids;
  }

  rx::observable <Fx> HelperControlSurface::fxAdded() const {
    return fxAddedSubject_.get_observable();
  }

  rx::observable <Fx> HelperControlSurface::fxRemoved() const {
    return fxRemovedSubject_.get_observable();
  }

  rx::observable <Project> HelperControlSurface::tracksReordered() const {
    return tracksReorderedSubject_.get_observable();
  }

  bool HelperControlSurface::isProbablyInputFx(Track track, int fxIndex, int paramIndex, double fxValue) const {
    const auto mediaTrack = track.mediaTrack();
    if (fxChainPairByMediaTrack_.count(mediaTrack)) {
      const auto& fxChainPair = fxChainPairByMediaTrack_.at(mediaTrack);
      const bool couldBeInputFx = fxIndex < fxChainPair.inputFxGuids.size();
      const bool couldBeOutputFx = fxIndex < fxChainPair.outputFxGuids.size();
      if (!couldBeInputFx && couldBeOutputFx) {
        return false;
      } else if (couldBeInputFx && !couldBeOutputFx) {
        return true;
      } else { // could be both
        if (paramIndex == -1) {
          // We don't have a parameter number at our disposal so we need to guess - we guess normal FX TODO
          return false;
        } else {
          // Compare parameter values (a heuristic but so what, it's just for MIDI learn)
          if (const auto outputFx = track.normalFxChain().fxByIndex(fxIndex)) {
            FxParameter outputFxParam = outputFx->parameterByIndex(paramIndex);
            bool isProbablyOutputFx = outputFxParam.reaperValue() == fxValue;
            return !isProbablyOutputFx;
          } else {
            return true;
          }
        }
      }
    } else {
      // Should not happen. In this case, an FX yet unknown to Realearn has sent a parameter change
      return false;
    }
  }

  bool HelperControlSurface::isProbablyInputFx(Track track, int fxIndex) const {
    return isProbablyInputFx(track, fxIndex, -1, -1);
  }

  rx::observable<Parameter*> HelperControlSurface::parameterValueChangedUnsafe() const {
    return fxParameterValueChanged().map([this](FxParameter fxParam) -> Parameter* {
          return new FxParameter(fxParam);
        })
        .merge(trackVolumeChanged().map([this](Track track) -> Parameter* {
          return new TrackVolume(track);
        }))
        .merge(trackPanChanged().map([this](Track track) -> Parameter* {
          return new TrackPan(track);
        }))
        .merge(trackSendVolumeChanged().map([this](TrackSend trackSend) -> Parameter* {
          return new TrackSendVolume(trackSend);
        }));
  }

  rx::observable<Parameter*> HelperControlSurface::parameterTouchedUnsafe() const {
    return fxParameterTouched().map([this](FxParameter fxParam) -> Parameter* {
          return new FxParameter(fxParam);
        })
        .merge(trackVolumeTouched().map([this](Track track) -> Parameter* {
          return new TrackVolume(track);
        }))
        .merge(trackPanTouched().map([this](Track track) -> Parameter* {
          return new TrackPan(track);
        }))
        .merge(trackSendVolumeTouched().map([this](TrackSend trackSend) -> Parameter* {
          return new TrackSendVolume(trackSend);
        }));
  }

  rx::observable <FxParameter> HelperControlSurface::fxParameterTouched() const {
    return fxParameterTouchedSubject_.get_observable();
  }

  rx::observable <Track> HelperControlSurface::trackVolumeTouched() const {
    return trackVolumeTouchedSubject_.get_observable();
  }

  rx::observable <Track> HelperControlSurface::trackPanTouched() const {
    return trackPanTouchedSubject_.get_observable();
  }

  rx::observable <TrackSend> HelperControlSurface::trackSendVolumeTouched() const {
    return trackSendVolumeTouchedSubject_.get_observable();
  }

  bool HelperControlSurface::trackParameterIsAutomated(Track track, string parameterName) const {
    if (track.isAvailable() && reaper::GetTrackEnvelopeByName(track.mediaTrack(), parameterName.c_str()) != nullptr) {
      // There's at least one automation lane for this parameter
      auto automationMode = track.effectiveAutomationMode();
      switch (automationMode) {
      case AutomationMode::Bypass:
      case AutomationMode::TrimRead:
      case AutomationMode::Write:
        // Is not automated
        return false;
      default:
        // Is automated
        return true;
      }
    } else {
      return false;
    }
  }
}