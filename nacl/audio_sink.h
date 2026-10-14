#ifndef AIRPLAY_AUDIO_SINK_H
#define AIRPLAY_AUDIO_SINK_H
#include <atomic>
#include <cstring>
#include <sstream>
#include "ppapi/cpp/audio.h"
#include "ppapi/cpp/module.h"
#include "ppapi/cpp/core.h"
#include "ppapi/cpp/audio_config.h"
#include "aacdecoder_lib.h"
#include "pcm_queue.h"

// Decode AAC explicitly, then send PCM through Pepper's audio output. This
// avoids depending on a second Samsung media player's AAC-ELD output path.
class AudioSink {
 public:
  explicit AudioSink(pp::Instance* instance) : instance_(instance) {}
  ~AudioSink() {Reset();}
  void Reset() {
    if(!output_.is_null()) output_.StopPlayback();
    output_=pp::Audio();
    if(decoder_) aacDecoder_Close(decoder_);
    decoder_=NULL;started_=false;queue_.Reset();synced_=false;
    played_.store(0);nonzero_played_.store(0);accepted_=errors_=0;
    logged_signal_=logged_output_=false;resample_pos_=0;
  }
  bool started() const {return started_;}
  void Start(int ct) {
    Reset();
    decoder_=aacDecoder_Open(TT_MP4_RAW,1);
    if(!decoder_) {Log("DIAG FAIL AAC decoder allocation");return;}
    unsigned char eld[]={0xf8,0xe8,0x50,0x00},lc[]={0x12,0x10};
    unsigned char* config=ct==8?eld:lc;UINT size=ct==8?4:2;
    AAC_DECODER_ERROR r=aacDecoder_ConfigRaw(decoder_,&config,&size);
    Log("DIAG software AAC config="+Number(r));
    if(r!=AAC_DEC_OK) {Reset();return;}
    aacDecoder_SetParam(decoder_,AAC_PCM_MIN_OUTPUT_CHANNELS,2);
    aacDecoder_SetParam(decoder_,AAC_PCM_MAX_OUTPUT_CHANNELS,2);
    tick_origin_=pp::Module::Get()->core()->GetTimeTicks();
    rate_=44100;
    uint32_t frames=pp::AudioConfig::RecommendSampleFrameCount(instance_,PP_AUDIOSAMPLERATE_44100,512);
    pp::AudioConfig audio_config(instance_,PP_AUDIOSAMPLERATE_44100,frames);
    if(audio_config.is_null()) {
      rate_=48000;frames=pp::AudioConfig::RecommendSampleFrameCount(instance_,PP_AUDIOSAMPLERATE_48000,512);
      audio_config=pp::AudioConfig(instance_,PP_AUDIOSAMPLERATE_48000,frames);
    }
    if(audio_config.is_null()) {Log("DIAG FAIL PCM audio configuration");Reset();return;}
    output_=pp::Audio(instance_,audio_config,&AudioSink::Render,this);
    started_=!output_.is_null() && output_.StartPlayback();
    Log(std::string("DIAG PCM output started=")+(started_?"true":"false")+" rate="+Number(rate_)+" buffer frames="+Number(frames));
  }
  void SetClock(double time,bool enabled) {
    int32_t now=static_cast<int32_t>((pp::Module::Get()->core()->GetTimeTicks()-tick_origin_)*1000);
    int32_t target=static_cast<int32_t>(time*1000)-now;
    int32_t old=clock_ms_.load(std::memory_order_relaxed),difference=target-old;
    // Keep audio time advancing between network callbacks. Small presentation
    // jitter changes the clock offset gradually, avoiding repeated gaps/cuts.
    if(!synced_.load() || difference>250 || difference< -250)old=target;
    else old+=std::max(-1,std::min(1,difference));
    clock_ms_.store(old,std::memory_order_release);synced_.store(enabled,std::memory_order_release);
  }
  void ClearQueued() {queue_.Clear();}
  void Feed(const std::string& bytes,double pts) {
    if(!started_ || !decoder_) return;
    UCHAR* input=reinterpret_cast<UCHAR*>(const_cast<char*>(bytes.data()));
    UINT size=bytes.size(),valid=size;
    AAC_DECODER_ERROR r=bytes.empty()?AAC_DEC_OK:aacDecoder_Fill(decoder_,&input,&size,&valid);
    if(r==AAC_DEC_OK) r=aacDecoder_DecodeFrame(decoder_,pcm_,sizeof(pcm_)/sizeof(pcm_[0]),bytes.empty()?AACDEC_CONCEAL:0);
    if(r!=AAC_DEC_OK) {
      if(++errors_<=5) Log("DIAG FAIL AAC decode="+Number(r)+" payload bytes="+Number(bytes.size()));
      return;
    }
    CStreamInfo* info=aacDecoder_GetStreamInfo(decoder_);
    if(!info || info->numChannels!=2 || info->frameSize<=0 || info->frameSize>2048 || info->sampleRate<=0) {
      Log("DIAG FAIL AAC decoded format");return;
    }
    int peak=0;
    for(int i=0;i<info->frameSize*2;++i) {int v=pcm_[i];if(v<0)v=-v;if(v>peak)peak=v;}
    if(++accepted_==1) Log("DIAG AAC decoded samples="+Number(info->frameSize)+" channels="+Number(info->numChannels)+" rate="+Number(info->sampleRate));
    if(peak>16 && !logged_signal_) {logged_signal_=true;Log("DIAG AAC non-silent PCM peak="+Number(peak));}
    unsigned output_frames=0;
    double step=static_cast<double>(info->sampleRate)/rate_;
    while(resample_pos_<info->frameSize && output_frames<4096) {
      int index=static_cast<int>(resample_pos_),next=std::min(index+1,info->frameSize-1);
      double fraction=resample_pos_-index;
      for(int c=0;c<2;++c)resampled_[2*output_frames+c]=static_cast<int16_t>(pcm_[index*2+c]+(pcm_[next*2+c]-pcm_[index*2+c])*fraction);
      ++output_frames;resample_pos_+=step;
    }
    resample_pos_-=info->frameSize;
    queue_.Push(resampled_,output_frames,pts,rate_,synced_.load());
    if(!logged_output_ && nonzero_played_.load(std::memory_order_acquire)>0) {
      logged_output_=true;Log("DIAG PCM output callback consumed non-silent audio");
    }
    if(accepted_%500==0) Log("DIAG AAC frames decoded="+Number(accepted_)+" queued_ms="+Number(queue_.queued()*1000/rate_)+" trimmed_frames="+Number(queue_.dropped())+" underrun_frames="+Number(queue_.empty())+" held_frames="+Number(queue_.held()));
  }
 private:
  static void Render(void* buffer,uint32_t bytes,void* opaque) {
    AudioSink* self=static_cast<AudioSink*>(opaque);
    int16_t* out=static_cast<int16_t*>(buffer);
    self->queue_.Render(out,bytes/4,self->rate_,pp::Module::Get()->core()->GetTimeTicks()-self->tick_origin_+self->clock_ms_.load(std::memory_order_acquire)/1000.0,self->synced_.load(std::memory_order_acquire));
    uint32_t consumed=bytes/2,signal=0;
    for(unsigned i=0;i<bytes/2;++i)if(out[i]>16 || out[i]< -16)++signal;
    self->played_.fetch_add(consumed,std::memory_order_relaxed);
    self->nonzero_played_.fetch_add(signal,std::memory_order_release);
  }
  void Log(const std::string& s) {instance_->PostMessage(pp::Var(s));}
  static std::string Number(int n) {std::ostringstream s;s<<n;return s.str();}
  PcmQueue queue_;
  std::atomic<int32_t> clock_ms_{0};
  std::atomic<bool> synced_{false};
  pp::Instance* instance_;
  pp::Audio output_;
  HANDLE_AACDECODER decoder_=NULL;
  INT_PCM pcm_[4096];int16_t resampled_[8192];
  std::atomic<uint32_t> played_{0},nonzero_played_{0};
  bool started_=false,logged_signal_=false,logged_output_=false;
  int rate_=44100,accepted_=0,errors_=0;
  double resample_pos_=0,tick_origin_=0;
};
#endif
