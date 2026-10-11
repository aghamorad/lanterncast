#ifndef AIRPLAY_VIDEO_SINK_H
#define AIRPLAY_VIDEO_SINK_H
#include <deque>
#include "h264_size.h"
#include "ppapi/cpp/core.h"
#include "ppapi/cpp/samsung/media_player_samsung.h"
#include "ppapi/cpp/samsung/es_data_source_samsung.h"
#include "ppapi/cpp/samsung/media_events_listener_samsung.h"
#include "ppapi/cpp/samsung/elementary_stream_listener_samsung.h"
class VideoSink : public pp::MediaEventsListener_Samsung,
                  public pp::ElementaryStreamListener_Samsung {
 public:
  explicit VideoSink(pp::Instance* instance) : instance_(instance), cb_(this), source_(instance) {}
  void Reset() {
    cb_.CancelAll(); AttachTo(NULL);
    player_=pp::MediaPlayer_Samsung(); stream_=pp::VideoElementaryStream_Samsung();
    source_=pp::ESDataSource_Samsung(instance_); frames_.clear(); config_.clear();
    started_=configured_=initializing_=attached_=ready_=pending_=enough_=playing_=shown_=false;
    accepted_=play_tries_=0;
  }
  bool started() const { return started_; }
  void OnTimeUpdate(PP_TimeTicks time) {
    if(time>0 && !shown_) { shown_=true; Log("DIAG video playback advancing");
      pp::Module::Get()->core()->CallOnMainThread(2000,cb_.NewCallback(&VideoSink::ReadTrack)); }
  }
  void OnError(PP_MediaPlayerError e) { Log("DIAG FAIL Samsung playback="+Number(e)); }
  void OnNeedData(int32_t) { enough_=false; Pump(); }
  void OnEnoughData() { enough_=true; }
  void OnSeekData(PP_TimeTicks) { enough_=false; }
  void Start(int width, int height) {
    if(started_) return;
    started_=true; width_=width; height_=height;
    source_.SetDuration(86400.0, cb_.NewCallback(&VideoSink::DurationSet));
    player_=pp::MediaPlayer_Samsung(instance_);
    AttachTo(&player_);
    PP_Rect rect={{0,0},{1920,1080}};
    int32_t d=player_.SetDisplayRect(rect,cb_.NewCallback(&VideoSink::DisplaySet));
    if(d!=PP_OK_COMPLETIONPENDING) DisplaySet(d);
    // Samsung defaults to stretching the video across the display rectangle.
    int32_t mode=player_.SetDisplayMode(PP_MEDIAPLAYERDISPLAYMODE_LETTERBOX,cb_.NewCallback(&VideoSink::DisplayModeSet));
    if(mode!=PP_OK_COMPLETIONPENDING) DisplayModeSet(mode);
    int32_t r=source_.AddStream<pp::VideoElementaryStream_Samsung>(cb_.NewCallbackWithOutput(&VideoSink::Added),this);
    if(r!=PP_OK_COMPLETIONPENDING && r!=PP_OK) Log("DIAG FAIL Samsung AddStream="+Number(r));
  }
  void Config(const std::string& config, int width, int height) {
    int w=width,h=height;
    if(!h264_size::Parse(config,&w,&h) && (w<=0 || h<=0 || w>8192 || h>8192)) {
      Log("DIAG FAIL video dimensions missing or invalid");return;
    }
    if(config==config_ && w==width_ && h==height_) return;
    config_=config;width_=w;height_=h;configured_=false;ready_=false;
    // Older firmware rejects SetDisplayMode. Fit the display rectangle itself
    // so its default stretch mode still preserves the source proportions.
    int display_w=1920,display_h=1080;
    if(static_cast<int64_t>(w)*1080>static_cast<int64_t>(h)*1920)
      display_h=static_cast<int64_t>(1920)*h/w;
    else display_w=static_cast<int64_t>(1080)*w/h;
    if(display_w<1) display_w=1;
    if(display_h<1) display_h=1;
    PP_Rect rect={{(1920-display_w)/2,(1080-display_h)/2},{display_w,display_h}};
    int32_t d=player_.SetDisplayRect(rect,cb_.NewCallback(&VideoSink::DisplaySet));
    if(d!=PP_OK_COMPLETIONPENDING) DisplaySet(d);
    Log("DIAG video source dimensions="+Number(w)+"x"+Number(h)+" fitted display="+Number(display_w)+"x"+Number(display_h));Configure();
  }
  void Feed(const std::string& bytes, double time) {
    if(frames_.size()>120) { Log("DIAG FAIL video queue full"); return; }
    Frame f; f.key=false;
    for(size_t i=0;i+4<bytes.size();++i)
      if(bytes[i]==0 && bytes[i+1]==0 && bytes[i+2]==0 && bytes[i+3]==1 && (bytes[i+4]&31)==5) f.key=true;
    f.bytes=bytes; f.time=time; frames_.push_back(f); Pump();
  }
 private:
  struct Frame {std::string bytes;double time;bool key;};
  void Log(const std::string& text) { instance_->PostMessage(pp::Var(text)); }
  static std::string Number(int n) { std::ostringstream s; s<<n;return s.str(); }
  void ReadTrack(int32_t) {player_.GetCurrentVideoTrackInfo(cb_.NewCallbackWithOutput(&VideoSink::TrackInfo));}
  void TrackInfo(int32_t r, PP_VideoTrackInfo info) {
    Log("DIAG video track result="+Number(r)+" width="+Number(info.size.width)+" height="+Number(info.size.height));
  }
  void Added(int32_t r, pp::VideoElementaryStream_Samsung stream) {
    Log("DIAG Samsung video stream="+Number(r));
    if(r!=PP_OK) return;
    stream_=stream; Configure();
  }
  void Configure() {
    if(stream_.is_null() || config_.empty() || configured_ || initializing_ || pending_) return;
    configured_=initializing_=true;
    stream_.SetVideoCodecType(PP_VIDEOCODEC_TYPE_SAMSUNG_H264);
    stream_.SetVideoCodecProfile(PP_VIDEOCODEC_PROFILE_SAMSUNG_H264_HIGH);
    PP_Size size={width_,height_}; stream_.SetVideoFrameSize(size);
    stream_.SetFrameRate(30,1);
    stream_.SetCodecExtraData(config_.size(),config_.data());
    int32_t r=stream_.InitializeDone(PP_STREAMINITIALIZATIONMODE_FULL,cb_.NewCallback(&VideoSink::Initialized));
    if(r!=PP_OK_COMPLETIONPENDING) Initialized(r);
  }
  void Initialized(int32_t r) {
    initializing_=false;
    Log("DIAG Samsung video initialized="+Number(r));
    if(r!=PP_OK) return;
    if(!configured_) {Configure();return;}
    if(attached_) {ready_=true;Pump();return;}
    int32_t a=player_.AttachDataSource(source_,cb_.NewCallback(&VideoSink::Attached));
    if(a!=PP_OK_COMPLETIONPENDING) Attached(a);
  }
  void Attached(int32_t r) {
    Log("DIAG Samsung source attached="+Number(r));
    attached_=r==PP_OK;ready_=attached_ && configured_;
    Configure();Pump();
  }
  void Pump() {
    if(!ready_ || pending_ || enough_ || frames_.empty()) return;
    Frame& f=frames_.front();
    PP_ESPacket p; memset(&p,0,sizeof(p));
    p.pts=p.dts=f.time; p.duration=1.0/30; p.is_key_frame=f.key?PP_TRUE:PP_FALSE;
    p.size=f.bytes.size(); p.buffer=&f.bytes[0];
    pending_=true;
    int32_t r=stream_.AppendPacket(p,cb_.NewCallback(&VideoSink::Appended));
    if(r!=PP_OK_COMPLETIONPENDING) Appended(r);
  }
  void Appended(int32_t r) {
    pending_=false;
    if(r==PP_ERROR_NOQUOTA) { Configure();pp::Module::Get()->core()->CallOnMainThread(10,cb_.NewCallback(&VideoSink::Retry)); return; }
    if(!frames_.empty()) frames_.pop_front();
    if(r!=PP_OK) {Log("DIAG FAIL Samsung AppendPacket="+Number(r));ready_=false;return;}
    Configure();
    if(accepted_ && accepted_%300==0) Log("DIAG video packets accepted="+Number(accepted_));
    if(++accepted_==1) Log("DIAG video first decoded packet accepted");
    if(!playing_ && accepted_>=3) {
      playing_=true;
      int32_t p=player_.Play(cb_.NewCallback(&VideoSink::Playing));
      if(p!=PP_OK_COMPLETIONPENDING) Playing(p);
    }
    // Schedule instead of recursing on synchronous append completion.
    pp::Module::Get()->core()->CallOnMainThread(0,cb_.NewCallback(&VideoSink::Retry));
  }
  void Retry(int32_t) { Pump(); }
  void DisplaySet(int32_t r) {Log("DIAG Samsung display="+Number(r));}
  void DisplayModeSet(int32_t r) {Log("DIAG Samsung aspect fit="+Number(r)+(r==PP_OK?"":" (using fitted display rectangle)"));}
  void DurationSet(int32_t r) {Log("DIAG Samsung duration="+Number(r));}
  void TryPlay(int32_t) {
    int32_t p=player_.Play(cb_.NewCallback(&VideoSink::Playing));
    if(p!=PP_OK_COMPLETIONPENDING) Playing(p);
  }
  void Playing(int32_t r) {
    if(r!=PP_OK && ++play_tries_<30) {
      pp::Module::Get()->core()->CallOnMainThread(200,cb_.NewCallback(&VideoSink::TryPlay));
    }
    if(r==PP_OK || play_tries_==1 || play_tries_==30) Log("DIAG Samsung Play="+Number(r));
  }
  pp::Instance* instance_;
  pp::CompletionCallbackFactory<VideoSink> cb_;
  pp::MediaPlayer_Samsung player_;
  pp::ESDataSource_Samsung source_;
  pp::VideoElementaryStream_Samsung stream_;
  std::deque<Frame> frames_;
  std::string config_;
  bool started_=false,configured_=false,initializing_=false,attached_=false,ready_=false,pending_=false,enough_=false,playing_=false,shown_=false;
  int width_=1920,height_=1080,accepted_=0,play_tries_=0;
};
#endif
