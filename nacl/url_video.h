#ifndef LANTERNCAST_URL_VIDEO_H
#define LANTERNCAST_URL_VIDEO_H
#include <memory>
#include <cmath>
#include "ppapi/cpp/samsung/url_data_source_samsung.h"
#include "ppapi/cpp/samsung/media_player_samsung.h"
#include "ppapi/cpp/samsung/media_events_listener_samsung.h"
class UrlVideo : public pp::MediaEventsListener_Samsung {
 public:
  explicit UrlVideo(pp::Instance* instance):instance_(instance),cb_(this) {}
  void Reset() {
    cb_.CancelAll();AttachTo(NULL);player_=pp::MediaPlayer_Samsung();source_.reset();
    active_=ready_=seeking_=false;duration_=position_=rate_=0;tries_=0;
  }
  bool active() const {return active_;}
  void Start(const std::string& url,double start,bool fraction,const std::string& cookie,const std::string& agent) {
    Reset();active_=true;start_=start;fraction_=fraction;rate_=1;
    player_=pp::MediaPlayer_Samsung(instance_);AttachTo(&player_);
    PP_Rect rect={{0,0},{1920,1080}};
    player_.SetDisplayRect(rect,cb_.NewCallback(&UrlVideo::Configured));
    player_.SetDisplayMode(PP_MEDIAPLAYERDISPLAYMODE_LETTERBOX,cb_.NewCallback(&UrlVideo::Configured));
    source_.reset(new pp::URLDataSource_Samsung(instance_,url));
    if(!cookie.empty()) source_->SetStreamingProperty(PP_STREAMINGPROPERTY_COOKIE,pp::Var(cookie),cb_.NewCallback(&UrlVideo::Configured));
    if(!agent.empty()) source_->SetStreamingProperty(PP_STREAMINGPROPERTY_USER_AGENT,pp::Var(agent),cb_.NewCallback(&UrlVideo::Configured));
    int32_t r=player_.AttachDataSource(*source_,cb_.NewCallback(&UrlVideo::Attached));
    if(r!=PP_OK_COMPLETIONPENDING) Attached(r);
    Log("DIAG direct video opening");
  }
  void Rate(double rate) {
    rate_=rate==0?0:1;
    if(ready_ && !seeking_) {int32_t r=rate_==0?player_.Pause(cb_.NewCallback(&UrlVideo::Configured)):player_.Play(cb_.NewCallback(&UrlVideo::Playing));if(r!=PP_OK_COMPLETIONPENDING) Configured(r);}
  }
  void Seek(double time) {if(ready_ && std::isfinite(time) && time>=0){seeking_=true;seek_target_=time;int32_t r=player_.Seek(time,cb_.NewCallback(&UrlVideo::Seeked));if(r!=PP_OK_COMPLETIONPENDING)Seeked(r);}}
  std::string Info() const {
    std::ostringstream s;s<<"<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
      <<"<key>duration</key><real>"<<duration_<<"</real><key>position</key><real>"<<position_
      <<"</real><key>rate</key><real>"<<(ready_?rate_:0)<<"</real><key>readyToPlay</key>"<<(ready_?"<true/>":"<false/>")
      <<"<key>playbackBufferEmpty</key>"<<(ready_?"<false/>":"<true/>")
      <<"<key>playbackLikelyToKeepUp</key>"<<(ready_?"<true/>":"<false/>")
      <<"<key>loadedTimeRanges</key><array/><key>seekableTimeRanges</key><array><dict><key>start</key><real>0</real><key>duration</key><real>"<<duration_<<"</real></dict></array></dict></plist>";return s.str();
  }
  std::string Scrub() const {std::ostringstream s;s<<"duration: "<<duration_<<"\r\nposition: "<<position_<<"\r\n";return s.str();}
  void OnTimeUpdate(PP_TimeTicks time) {if(!seeking_)position_=time;}
  void OnEnded() {Reset();Log("DIAG direct video ended");}
  void OnError(PP_MediaPlayerError error) {active_=ready_=false;Log("DIAG FAIL direct video error="+Number(error));}
 private:
  void Log(const std::string& s) {instance_->PostMessage(pp::Var(s));}
  static std::string Number(int n) {std::ostringstream s;s<<n;return s.str();}
  void Configured(int32_t r) {if(r!=PP_OK && r!=PP_ERROR_NOTSUPPORTED)Log("DIAG direct video operation="+Number(r));}
  void Attached(int32_t r) {
    Log("DIAG direct video attached="+Number(r));if(r!=PP_OK)return;ready_=true;
    int32_t d=player_.GetDuration(cb_.NewCallbackWithOutput(&UrlVideo::Duration));
    if(d!=PP_OK_COMPLETIONPENDING && d!=PP_OK) Play(0);
    player_.GetCurrentVideoTrackInfo(cb_.NewCallbackWithOutput(&UrlVideo::Track));
  }
  void Duration(int32_t r,double d) {if(r==PP_OK && std::isfinite(d) && d>0)duration_=d;if(start_>0)Seek(fraction_?start_*duration_:start_);else Play(0);}
  void Seeked(int32_t r) {seeking_=false;if(r==PP_OK)position_=seek_target_;else Configured(r);if(rate_>0)Play(0);}
  void Track(int32_t r,PP_VideoTrackInfo t) {
    if(r!=PP_OK || t.size.width<=0 || t.size.height<=0)return;
    int w=1920,h=1080;
    if(int64_t(t.size.width)*1080>int64_t(t.size.height)*1920)h=int64_t(1920)*t.size.height/t.size.width;
    else w=int64_t(1080)*t.size.width/t.size.height;
    PP_Rect rect={{(1920-w)/2,(1080-h)/2},{w,h}};player_.SetDisplayRect(rect,cb_.NewCallback(&UrlVideo::Configured));
  }
  void Play(int32_t) {int32_t r=player_.Play(cb_.NewCallback(&UrlVideo::Playing));if(r!=PP_OK_COMPLETIONPENDING)Playing(r);}
  void Playing(int32_t r) {
    if(r==PP_OK){Log("DIAG direct video playing");return;}
    if(++tries_<60)pp::Module::Get()->core()->CallOnMainThread(200,cb_.NewCallback(&UrlVideo::Play));
    else Log("DIAG FAIL direct video Play="+Number(r));
  }
  pp::Instance* instance_;pp::CompletionCallbackFactory<UrlVideo> cb_;
  pp::MediaPlayer_Samsung player_;std::unique_ptr<pp::URLDataSource_Samsung> source_;
  bool active_=false,ready_=false,fraction_=false,seeking_=false;double duration_=0,position_=0,rate_=0,start_=0,seek_target_=0;int tries_=0;
};
#endif
