// Pepper hardware H.264 decode and GLES presentation.
// Rendering follows Chromium's Pepper 47 video_decode example (BSD license).
#ifndef AIRPLAY_VIDEO_SINK_H
#define AIRPLAY_VIDEO_SINK_H
#include <deque>
#include <map>
#include "native_video_sink.h"
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include "ppapi/c/ppb_opengles2.h"
#include "ppapi/cpp/graphics_3d.h"
#include "ppapi/cpp/graphics_3d_client.h"
#include "ppapi/cpp/video_decoder.h"
class VideoSink : public pp::Graphics3DClient {
 public:
  explicit VideoSink(pp::Instance* instance)
      : pp::Graphics3DClient(instance), instance_(instance), cb_(this), fallback_(instance) {}
  void Graphics3DContextLost() { if(using_fallback_)return;ready_ = false; Log("DIAG FAIL video graphics context lost"); }
  void Reset() {
    cb_.CancelAll();fallback_.Reset();decoder_=pp::VideoDecoder();
    if(!context_.is_null())instance_->BindGraphics(pp::Graphics3D());
    context_=pp::Graphics3D();
    frames_.clear();times_.clear();programs_[0]=programs_[1]=0;last_program_=0;last_texture_=0;last_vw_=last_vh_=last_tw_=last_th_=0;
    started_=ready_=decoding_=using_fallback_=waiting_key_=has_next_=painting_=false;clock_=-1;shown_=decode_id_=skipped_=0;stats_tick_=swap_total_=0;
  }
  bool started() const { return started_; }
  bool has_clock() const {return using_fallback_?fallback_.has_clock():clock_>=0;}
  double PlaybackTime() const {return using_fallback_?fallback_.PlaybackTime():clock_<0?0:clock_+std::min(0.12,pp::Module::Get()->core()->GetTimeTicks()-clock_tick_);}
  void Config(const std::string& codec,int width,int height) {
    codec_=codec;h264_size::Parse(codec,&width,&height);
    if(using_fallback_)fallback_.Config(codec,width,height);
  }
  void Start(int width, int height) {
    if (started_) return;
    started_ = true;
    width_ = width; height_ = height;
    gl_ = static_cast<const PPB_OpenGLES2*>(pp::Module::Get()->GetBrowserInterface(PPB_OPENGLES2_INTERFACE));
    if (!gl_) { Log("DIAG FAIL GLES2 absent"); return; }
    int32_t attrs[] = {PP_GRAPHICS3DATTRIB_ALPHA_SIZE, 0,
      PP_GRAPHICS3DATTRIB_RED_SIZE, 8,PP_GRAPHICS3DATTRIB_GREEN_SIZE, 8,PP_GRAPHICS3DATTRIB_BLUE_SIZE, 8,
      PP_GRAPHICS3DATTRIB_WIDTH, width, PP_GRAPHICS3DATTRIB_HEIGHT, height,
      PP_GRAPHICS3DATTRIB_NONE};
    context_ = pp::Graphics3D(instance_, attrs);
    if (context_.is_null() || !instance_->BindGraphics(context_)) {
      Log("DIAG FAIL video graphics bind"); return;
    }
    int32_t swap_attrs[]={PP_GRAPHICS3DATTRIB_SWAP_BEHAVIOR,PP_GRAPHICS3DATTRIB_BUFFER_DESTROYED,PP_GRAPHICS3DATTRIB_NONE};
    context_.SetAttribs(swap_attrs);
    static const float vertices[] = {-1,-1, -1,1, 1,-1, 1,1, 0,1, 0,0, 1,1, 1,0};
    GLuint buffer;
    gl_->GenBuffers(C(), 1, &buffer);
    gl_->BindBuffer(C(), GL_ARRAY_BUFFER, buffer);
    gl_->BufferData(C(), GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    decoder_ = pp::VideoDecoder(instance_);
    int32_t r = decoder_.Initialize(context_, PP_VIDEOPROFILE_H264HIGH,
          PP_HARDWAREACCELERATION_ONLY, 0,
          cb_.NewCallback(&VideoSink::Initialized));
    if (r != PP_OK_COMPLETIONPENDING) Initialized(r);
  }
  void Feed(const std::string& bytes,double time) {
    if(using_fallback_){fallback_.Feed(bytes,time);return;}
    last_pts_=time;
    bool key=false;
    for(size_t i=0;i+4<bytes.size();++i)if(bytes.compare(i,4,std::string("\0\0\0\1",4))==0 && (bytes[i+4]&31)==5){key=true;break;}
    if(!frames_.empty() && time-frames_.front().time>0.25)waiting_key_=true;
    if(waiting_key_) {
      if(!key)return;
      if(decoding_ && !frames_.empty())frames_.erase(frames_.begin()+1,frames_.end());else frames_.clear();
      waiting_key_=false;Log("DIAG direct decoder backlog skipped at keyframe");
    }
    Frame f={bytes,time};frames_.push_back(f); DecodeNext();
  }
 private:
  PP_Resource C() { return context_.pp_resource(); }
  void Log(const std::string& text) { instance_->PostMessage(pp::Var(text)); }
  void Initialized(int32_t result) {
    Log("DIAG video decoder Initialize=" + Number(result));
    if(result!=PP_OK) {
      instance_->BindGraphics(pp::Graphics3D());context_=pp::Graphics3D();decoder_=pp::VideoDecoder();using_fallback_=true;
      fallback_.Start(width_,height_);if(!codec_.empty())fallback_.Config(codec_,0,0);
      for(auto& f:frames_)fallback_.Feed(f.bytes,f.time);frames_.clear();return;
    }
    ready_ = result == PP_OK;
    if (ready_) { GetPicture(); DecodeNext(); }
  }
  static std::string Number(int n) { std::ostringstream s; s << n; return s.str(); }
  void DecodeNext() {
    if (!ready_ || decoding_ || frames_.empty() || times_.size()>=(clock_<0?24u:6u)) return;
    decoding_ = true;
    const std::string& bytes = frames_.front().bytes;
    times_[decode_id_+1]=frames_.front().time;
    int32_t r = decoder_.Decode(++decode_id_, bytes.size(), bytes.data(),
      cb_.NewCallback(&VideoSink::Decoded));
    if (r != PP_OK_COMPLETIONPENDING) Decoded(r);
  }
  void Decoded(int32_t r) {
    decoding_ = false;
    if (!frames_.empty()) frames_.pop_front();
    if (r != PP_OK) { ready_=false; Log("DIAG FAIL video Decode="+Number(r)); return; }
    DecodeNext();
  }
  void GetPicture() {
    int32_t r = decoder_.GetPicture(cb_.NewCallbackWithOutput(&VideoSink::Picture));
    if (r != PP_OK_COMPLETIONPENDING) Log("DIAG FAIL video GetPicture="+Number(r));
  }
  GLuint Program(GLenum target) {
    int index = target == GL_TEXTURE_EXTERNAL_OES ? 1 : 0;
    if (programs_[index]) return programs_[index];
    const char* vs = "attribute vec2 a_position;attribute vec2 a_texCoord;varying vec2 uv;uniform vec2 scale;void main(){gl_Position=vec4(a_position,0.,1.);uv=a_texCoord*scale;}";
    const char* fs = index ? "#extension GL_OES_EGL_image_external : require\nprecision mediump float;varying vec2 uv;uniform samplerExternalOES tex;void main(){gl_FragColor=texture2D(tex,uv);}" : "precision mediump float;varying vec2 uv;uniform sampler2D tex;void main(){gl_FragColor=texture2D(tex,uv);}";
    GLuint prog=gl_->CreateProgram(C());
    for(int i=0;i<2;++i) {
      GLuint sh=gl_->CreateShader(C(), i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
      const char* src=i ? fs : vs;
      gl_->ShaderSource(C(),sh,1,&src,NULL); gl_->CompileShader(C(),sh);
      GLint ok=0; gl_->GetShaderiv(C(),sh,GL_COMPILE_STATUS,&ok);
      if (!ok) { char error[512]; gl_->GetShaderInfoLog(C(),sh,sizeof(error),NULL,error); Log(std::string("DIAG FAIL shader: ")+error); }
      gl_->AttachShader(C(),prog,sh); gl_->DeleteShader(C(),sh);
    }
    gl_->LinkProgram(C(),prog);
    programs_[index]=prog;
    pos_[index]=gl_->GetAttribLocation(C(),prog,"a_position");uv_[index]=gl_->GetAttribLocation(C(),prog,"a_texCoord");
    tex_[index]=gl_->GetUniformLocation(C(),prog,"tex");scale_[index]=gl_->GetUniformLocation(C(),prog,"scale");return prog;
  }
  void Picture(int32_t r, PP_VideoPicture p) {
    if(r != PP_OK) { Log("DIAG FAIL video Picture="+Number(r)); return; }
    if(p.texture_target==0){decoder_.RecyclePicture(p);GetPicture();return;}
    auto stamp=times_.find(p.decode_id);double pts=stamp==times_.end()?last_pts_:stamp->second;
    if(stamp!=times_.end())times_.erase(stamp);
    if(has_next_){decoder_.RecyclePicture(next_picture_);++skipped_;}
    next_picture_=p;next_time_=pts;has_next_=true;
    GetPicture();DecodeNext();
    if(!painting_)Paint();
  }
  void Paint() {
    swap_tick_=pp::Module::Get()->core()->GetTimeTicks();
    painting_=true;picture_=next_picture_;picture_time_=next_time_;has_next_=false;
    PP_VideoPicture p=picture_;
    GLuint prog=Program(p.texture_target);
    int index=p.texture_target==GL_TEXTURE_EXTERNAL_OES?1:0;
    if(last_program_!=prog) {
      gl_->UseProgram(C(),prog);
      gl_->EnableVertexAttribArray(C(),pos_[index]);gl_->VertexAttribPointer(C(),pos_[index],2,GL_FLOAT,GL_FALSE,0,0);
      gl_->EnableVertexAttribArray(C(),uv_[index]);gl_->VertexAttribPointer(C(),uv_[index],2,GL_FLOAT,GL_FALSE,0,reinterpret_cast<void*>(8*sizeof(float)));
      gl_->Uniform1i(C(),tex_[index],0);gl_->ActiveTexture(C(),GL_TEXTURE0);
      last_program_=prog;last_texture_=0;last_vw_=0;
    }

    int w=width_,h=height_;
    if(int64_t(p.visible_rect.size.width)*height_>int64_t(p.visible_rect.size.height)*width_)h=int64_t(width_)*p.visible_rect.size.height/p.visible_rect.size.width;
    else w=int64_t(height_)*p.visible_rect.size.width/p.visible_rect.size.height;
    if(last_vw_!=p.visible_rect.size.width || last_vh_!=p.visible_rect.size.height || last_tw_!=p.texture_size.width || last_th_!=p.texture_size.height){
      gl_->Uniform2f(C(),scale_[index],static_cast<float>(p.visible_rect.size.width)/p.texture_size.width,static_cast<float>(p.visible_rect.size.height)/p.texture_size.height);
      gl_->Viewport(C(),(width_-w)/2,(height_-h)/2,w,h);gl_->ClearColor(C(),0,0,0,1);
      last_vw_=p.visible_rect.size.width;last_vh_=p.visible_rect.size.height;last_tw_=p.texture_size.width;last_th_=p.texture_size.height;
    }
    if(w!=width_ || h!=height_)gl_->Clear(C(),GL_COLOR_BUFFER_BIT);
    if(last_texture_!=p.texture_id){gl_->BindTexture(C(),p.texture_target,p.texture_id);last_texture_=p.texture_id;}
    gl_->DrawArrays(C(),GL_TRIANGLE_STRIP,0,4);
    if(shown_==0){int32_t e=gl_->GetError(C());if(e)Log("DIAG FAIL GLES draw="+Number(e));}
    int32_t swap=context_.SwapBuffers(cb_.NewCallback(&VideoSink::Swapped));
    if(swap != PP_OK_COMPLETIONPENDING) Swapped(swap);
  }
  void Swapped(int32_t r) {
    decoder_.RecyclePicture(picture_);painting_=false;
    if(r != PP_OK) { Log("DIAG FAIL video swap="+Number(r)); return; }
    clock_=picture_time_;clock_tick_=pp::Module::Get()->core()->GetTimeTicks();
    swap_total_+=clock_tick_-swap_tick_;
    if(++shown_==1){stats_tick_=clock_tick_;Log("DIAG video playback advancing (direct hardware decoder)");}
    if(shown_%150==0){
      double elapsed=clock_tick_-stats_tick_;
      Log("DIAG direct decoder frames="+Number(shown_)+" queue="+Number(frames_.size())+" lag_ms="+Number(static_cast<int>((last_pts_-clock_)*1000))+" fps="+Number(elapsed>0?149/elapsed:0)+" swap_ms="+Number(swap_total_*1000/150)+" skipped="+Number(skipped_));
      stats_tick_=clock_tick_;swap_total_=0;
    }
    if(has_next_)Paint();DecodeNext();
  }
  pp::Instance* instance_;
  pp::CompletionCallbackFactory<VideoSink> cb_;
  const PPB_OpenGLES2* gl_=NULL;
  pp::Graphics3D context_;
  pp::VideoDecoder decoder_;
  struct Frame {std::string bytes;double time;};
  std::deque<Frame> frames_;std::map<int32_t,double> times_;
  NativeVideoSink fallback_;bool using_fallback_=false,waiting_key_=false;std::string codec_;
  double clock_=-1,clock_tick_=0,last_pts_=0,picture_time_=0;
  double stats_tick_=0,swap_tick_=0,swap_total_=0;int skipped_=0;
  PP_VideoPicture picture_,next_picture_;double next_time_=0;bool has_next_=false,painting_=false;
  GLuint programs_[2]={0,0},last_program_=0,last_texture_=0;GLint pos_[2],uv_[2],tex_[2],scale_[2];int last_vw_=0,last_vh_=0,last_tw_=0,last_th_=0;
  bool started_=false, ready_=false, decoding_=false;
  int width_=1920,height_=1080,decode_id_=0,shown_=0;
};
#endif
