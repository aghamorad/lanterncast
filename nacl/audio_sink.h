#ifndef AIRPLAY_AUDIO_SINK_H
#define AIRPLAY_AUDIO_SINK_H
#include <deque>
#include "ppapi/cpp/core.h"
#include "ppapi/cpp/samsung/media_player_samsung.h"
#include "ppapi/cpp/samsung/es_data_source_samsung.h"
#include "ppapi/cpp/samsung/media_events_listener_samsung.h"
#include "ppapi/cpp/samsung/elementary_stream_listener_samsung.h"
class AudioSink : public pp::MediaEventsListener_Samsung,
                  public pp::ElementaryStreamListener_Samsung {
 public:
  explicit AudioSink(pp::Instance* instance) : instance_(instance), cb_(this), source_(instance) {}
  void Reset() {
    cb_.CancelAll(); AttachTo(NULL);
    player_=pp::MediaPlayer_Samsung(); stream_=pp::AudioElementaryStream_Samsung();
    source_=pp::ESDataSource_Samsung(instance_); frames_.clear(); config_.clear();
    started_=configured_=ready_=pending_=enough_=playing_=shown_=false;
    accepted_=play_tries_=0;
  }
  bool started() const { return started_; }
  void OnTimeUpdate(PP_TimeTicks time) {
    if(time>0 && !shown_) { shown_=true; Log("DIAG audio playback advancing"); }
  }
  void OnError(PP_MediaPlayerError e) { Log("DIAG FAIL Samsung audio playback="+Number(e)); }
  void OnNeedData(int32_t) { enough_=false; Pump(); }
  void OnEnoughData() { enough_=true; }
  void OnSeekData(PP_TimeTicks) { enough_=false; }
  void Start(int ct) {
    Reset(); ct_=ct; config_= ct==8?std::string("\xf8\xe8\x50\0",4):std::string("\x12\x10",2);
    if(started_) return;
    started_=true;
    source_.SetDuration(86400.0, cb_.NewCallback(&AudioSink::DurationSet));
    player_=pp::MediaPlayer_Samsung(instance_,pp::MediaPlayer_Samsung::DO_NOT_BIND_TO_INSTANCE);
    AttachTo(&player_);
    int32_t r=source_.AddStream<pp::AudioElementaryStream_Samsung>(cb_.NewCallbackWithOutput(&AudioSink::Added),this);
    if(r!=PP_OK_COMPLETIONPENDING && r!=PP_OK) Log("DIAG FAIL Samsung AddStream="+Number(r));
  }
  void Feed(const std::string& bytes, double time) {
    if(frames_.size()>120) { Log("DIAG FAIL audio queue full"); return; }
    Frame f; f.key=true;
    f.bytes=bytes; f.time=time; frames_.push_back(f); Pump();
  }
 private:
  struct Frame {std::string bytes;double time;bool key;};
  void Log(const std::string& text) { instance_->PostMessage(pp::Var(text)); }
  static std::string Number(int n) { std::ostringstream s; s<<n;return s.str(); }
  void Added(int32_t r, pp::AudioElementaryStream_Samsung stream) {
    Log("DIAG Samsung audio stream="+Number(r));
    if(r!=PP_OK) return;
    stream_=stream; Configure();
  }
  void Configure() {
    if(stream_.is_null() || config_.empty() || configured_) return;
    configured_=true;
    stream_.SetAudioCodecType(PP_AUDIOCODEC_TYPE_SAMSUNG_AAC);
    stream_.SetAudioCodecProfile(ct_==8?PP_AUDIOCODEC_PROFILE_SAMSUNG_AAC_ELD:PP_AUDIOCODEC_PROFILE_SAMSUNG_AAC_LOW);
    stream_.SetChannelLayout(PP_CHANNEL_LAYOUT_SAMSUNG_STEREO);
    stream_.SetSamplesPerSecond(44100); stream_.SetBitsPerChannel(16);
    stream_.SetCodecExtraData(config_.size(),config_.data());
    int32_t r=stream_.InitializeDone(PP_STREAMINITIALIZATIONMODE_FULL,cb_.NewCallback(&AudioSink::Initialized));
    if(r!=PP_OK_COMPLETIONPENDING) Initialized(r);
  }
  void Initialized(int32_t r) {
    Log("DIAG Samsung audio initialized="+Number(r));
    if(r!=PP_OK) return;
    int32_t a=player_.AttachDataSource(source_,cb_.NewCallback(&AudioSink::Attached));
    if(a!=PP_OK_COMPLETIONPENDING) Attached(a);
  }
  void Attached(int32_t r) {
    Log("DIAG Samsung audio source attached="+Number(r));
    ready_=r==PP_OK; Pump();
  }
  void Pump() {
    if(!ready_ || pending_ || enough_ || frames_.empty()) return;
    Frame& f=frames_.front();
    PP_ESPacket p; memset(&p,0,sizeof(p));
    p.pts=p.dts=f.time; p.duration=static_cast<double>(ct_==8?480:1024)/44100; p.is_key_frame=f.key?PP_TRUE:PP_FALSE;
    p.size=f.bytes.size(); p.buffer=&f.bytes[0];
    pending_=true;
    int32_t r=stream_.AppendPacket(p,cb_.NewCallback(&AudioSink::Appended));
    if(r!=PP_OK_COMPLETIONPENDING) Appended(r);
  }
  void Appended(int32_t r) {
    pending_=false;
    if(r==PP_ERROR_NOQUOTA) { pp::Module::Get()->core()->CallOnMainThread(10,cb_.NewCallback(&AudioSink::Retry)); return; }
    if(!frames_.empty()) frames_.pop_front();
    if(r!=PP_OK) {Log("DIAG FAIL Samsung AppendPacket="+Number(r));ready_=false;return;}
    if(accepted_ && accepted_%300==0) Log("DIAG audio packets accepted="+Number(accepted_));
    if(++accepted_==1) Log("DIAG audio first decoded packet accepted");
    if(!playing_ && accepted_>=3) {
      playing_=true;
      int32_t p=player_.Play(cb_.NewCallback(&AudioSink::Playing));
      if(p!=PP_OK_COMPLETIONPENDING) Playing(p);
    }
    // Schedule instead of recursing on synchronous append completion.
    pp::Module::Get()->core()->CallOnMainThread(0,cb_.NewCallback(&AudioSink::Retry));
  }
  void Retry(int32_t) { Pump(); }
  void DisplaySet(int32_t r) {Log("DIAG Samsung display="+Number(r));}
  void DurationSet(int32_t r) {Log("DIAG Samsung audio duration="+Number(r));}
  void TryPlay(int32_t) {
    int32_t p=player_.Play(cb_.NewCallback(&AudioSink::Playing));
    if(p!=PP_OK_COMPLETIONPENDING) Playing(p);
  }
  void Playing(int32_t r) {
    if(r!=PP_OK && ++play_tries_<30) {
      pp::Module::Get()->core()->CallOnMainThread(200,cb_.NewCallback(&AudioSink::TryPlay));
    }
    if(r==PP_OK || play_tries_==1 || play_tries_==30) Log("DIAG Samsung audio Play="+Number(r));
  }
  pp::Instance* instance_;
  pp::CompletionCallbackFactory<AudioSink> cb_;
  pp::MediaPlayer_Samsung player_;
  pp::ESDataSource_Samsung source_;
  pp::AudioElementaryStream_Samsung stream_;
  std::deque<Frame> frames_;
  std::string config_;
  bool started_=false,configured_=false,ready_=false,pending_=false,enough_=false,playing_=false,shown_=false;
  int width_=1920,height_=1080,accepted_=0,play_tries_=0,ct_=8;
};
#endif
