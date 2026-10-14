// AirPlay receiver — NaCl (Pepper) module.
//
// M1 — does the TV's Pepper runtime load a NaCl module and let it paint?
//      PASS 2026-10-11: loads; BindGraphics ok; Flush -> PP_OK; seen on screen.
// M2 — do raw sockets work (TCP listening socket + UDP multicast)?
//      PASS 2026-10-11: TCP :7000 accepted the Mac; UDP :7001 joined
//      239.255.255.250 and the module's own datagram came back.
// M3 — mDNS: advertise _airplay._tcp and _raop._tcp so the Mac's AirPlay menu
//      lists this TV at all.
//
// Everything this module learns is reported to JavaScript as a plain string
// (see index.html, which forwards it to the Mac).  The module never claims a
// milestone passed; it reports what actually happened.
//
// Build:  see Makefile / scripts/build.sh  (PNaCl -> pnacl-translate -arch arm)

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "ppapi/c/pp_bool.h"
#include "ppapi/c/pp_errors.h"
#include "ppapi/c/pp_var.h"
#include "ppapi/c/ppb_net_address.h"
#include "ppapi/c/ppb_udp_socket.h"
#include "ppapi/cpp/completion_callback.h"
#include "ppapi/cpp/graphics_2d.h"
#include "ppapi/cpp/image_data.h"
#include "ppapi/cpp/instance.h"
#include "ppapi/cpp/module.h"
#include "ppapi/cpp/net_address.h"
#include "ppapi/cpp/point.h"
#include "ppapi/cpp/rect.h"
#include "ppapi/cpp/size.h"
#include "ppapi/cpp/tcp_socket.h"
#include "ppapi/cpp/udp_socket.h"
#include "ppapi/cpp/var.h"
#include "ppapi/cpp/view.h"
// Not in ppapi/cpp/ in this SDK — the factory lives under ppapi/utility/.
#include "ppapi/utility/completion_callback_factory.h"

// FairPlay is plain C (see fairplay.h); this is the one place it is reached.
#include "fairplay.h"

// SETUP arrives as an Apple binary plist and has to be answered with one.
#include "bplist.h"
#include "video_sink.h"
#include "audio_sink.h"
#include "url_video.h"
#include "rtp_queue.h"
#include "ppapi/cpp/var_array_buffer.h"
#include "ppapi/cpp/core.h"
extern "C" {
#include "aes.h"
int crypto_hash(unsigned char*, const unsigned char*, unsigned long long);
}

namespace {

// Ports the M2 test listens on.  Deliberately high and otherwise-unused so the
// Mac can reach them from the LAN with a plain nc/telnet probe.
const uint16_t kTcpPort = 7000;
const uint16_t kUdpPort = 7001;

// SSDP's multicast group — the same group UxPlay/airplay uses for discovery.
// 239.255.255.250 in network byte order.
const uint8_t kMulticastAddr[4] = {239, 255, 255, 250};

// mDNS.  224.0.0.251 is the group every Bonjour client listens on, and 5353 is
// the port discovery expects a responder to answer from.
const uint8_t kMdnsAddr[4] = {224, 0, 0, 251};
const uint16_t kMdnsPort = 5353;

// M5/M6 stream endpoints.  Bound once at startup rather than in response to
// SETUP: a mirror data port is a *listener* the client dials into, so it has to
// exist before the reply that advertises it goes out.
const uint16_t kNtpPort = 7002;    // UDP, timing
const uint16_t kVideoPort = 7100;  // TCP, mirroring (stream type 110)
const uint16_t kAudioPort = 7101;  // UDP RTP audio (stream type 96); legacy TCP listener retained
// Audio RTP and receiver control use separate UDP sockets.
const int kStreamCount = 2;
const uint16_t kStreamPorts[kStreamCount] = {kVideoPort, kAudioPort};
const char* const kStreamNames[kStreamCount] = {"video", "audio"};

// What this module advertises.  Deliberately fixed: the Mac caches the
// deviceid, so generating a fresh one each launch would re-add the TV every
// time instead of recognising it.
const char kServiceName[] = "LanternCast";
std::string kHostName = "LanternCast.local";
std::string kDeviceId = "00:00:00:00:00:00";
std::string kRaopMac = "000000000000";        // same, colon-free, for _raop
const char kPairingId[] = "b1a0c9e2-4d3f-4c8a-9e10-5f7a2c3d4e5f";
const char kSrcVersion[] = "220.68";
const char kModel[] = "AppleTV3,2";

// Pepper wants all PP_NetAddress_IPv4 members in network byte order.  The SDK
// hand-rolls this rather than pulling in a socket header (see the SDK's own
// examples/api/socket/echo_server.cc).
uint16_t Htons(uint16_t hostshort) {
  uint16_t host = 1;
  if (*reinterpret_cast<uint8_t*>(&host)) {
    // Little-endian host: swap.
    return static_cast<uint16_t>((hostshort >> 8) | (hostshort << 8));
  }
  return hostshort;
}

pp::NetAddress MakeIPv4(pp::Instance* instance, const uint8_t addr[4],
                        uint16_t port) {
  PP_NetAddress_IPv4 ipv4;
  ipv4.port = Htons(port);
  for (int i = 0; i < 4; ++i)
    ipv4.addr[i] = addr[i];
  return pp::NetAddress(instance, ipv4);
}

// Space-separated lowercase hex, capped so a long buffer cannot flood the log.
std::string HexBytes(const std::string& data, size_t max) {
  std::string out;
  char b[8];
  size_t n = (data.size() < max) ? data.size() : max;
  for (size_t i = 0; i < n; ++i) {
    snprintf(b, sizeof(b), "%02x", static_cast<unsigned char>(data[i]));
    if (i) out += ' ';
    out += b;
  }
  if (data.size() > n) out += " ...";
  return out;
}

int HexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}

// A plist node rendered for the log.
std::string NodeText(const Bplist* n) {
  if (!n) return "(absent)";
  std::ostringstream oss;
  switch (n->kind()) {
    case Bplist::kUint:   oss << n->uint(); break;
    case Bplist::kString: oss << '"' << n->str() << '"'; break;
    case Bplist::kData:
      oss << "data[" << n->str().size() << "] " << HexBytes(n->str(), 8);
      break;
    case Bplist::kArray:  oss << "array[" << n->count() << "]"; break;
    case Bplist::kDict:   oss << "dict[" << n->count() << "]"; break;
    default:              oss << "(null)"; break;
  }
  return oss.str();
}

const char* ResultName(int32_t r) {
  if (r == PP_OK)
    return "PP_OK";
  if (r == PP_OK_COMPLETIONPENDING)
    return "PP_OK_COMPLETIONPENDING";
  if (r == PP_ERROR_FAILED)
    return "PP_ERROR_FAILED";
  if (r == PP_ERROR_BADRESOURCE)
    return "PP_ERROR_BADRESOURCE";
  if (r == PP_ERROR_BADARGUMENT)
    return "PP_ERROR_BADARGUMENT";
  if (r == PP_ERROR_NOACCESS)
    return "PP_ERROR_NOACCESS";
  if (r == PP_ERROR_NOMEMORY)
    return "PP_ERROR_NOMEMORY";
  if (r == PP_ERROR_RESOURCE_FAILED)
    return "PP_ERROR_RESOURCE_FAILED";
  if (r == PP_ERROR_NOTSUPPORTED)
    return "PP_ERROR_NOTSUPPORTED";
  return "?";
}

// ---- mDNS / DNS wire format ------------------------------------------------
//
// Built by hand.  Pepper offers no service-registration API, and the record
// set below is fixed, so writing the wire format is smaller than working
// around its absence.  Names go out uncompressed: that is legal DNS, just
// fatter than mDNSResponder would emit.

void PutU16(std::vector<uint8_t>* b, uint16_t v) {
  b->push_back(static_cast<uint8_t>(v >> 8));
  b->push_back(static_cast<uint8_t>(v & 0xff));
}

void PutU32(std::vector<uint8_t>* b, uint32_t v) {
  PutU16(b, static_cast<uint16_t>(v >> 16));
  PutU16(b, static_cast<uint16_t>(v & 0xffff));
}

void PutName(std::vector<uint8_t>* b, const std::string& dotted) {
  size_t start = 0;
  while (start < dotted.size()) {
    size_t dot = dotted.find('.', start);
    if (dot == std::string::npos)
      dot = dotted.size();
    b->push_back(static_cast<uint8_t>(dot - start));
    b->insert(b->end(), dotted.begin() + start, dotted.begin() + dot);
    start = dot + 1;
  }
  b->push_back(0);
}

void PutRecord(std::vector<uint8_t>* b, const std::string& name, uint16_t type,
               const std::vector<uint8_t>& rdata, uint32_t ttl) {
  PutName(b, name);
  PutU16(b, type);
  PutU16(b, 1);                                    // class IN
  PutU32(b, ttl);
  PutU16(b, static_cast<uint16_t>(rdata.size()));
  b->insert(b->end(), rdata.begin(), rdata.end());
}

void AddPtr(std::vector<uint8_t>* b, const std::string& name,
            const std::string& target, uint32_t ttl = 4500) {
  std::vector<uint8_t> rd;
  PutName(&rd, target);
  PutRecord(b, name, 12, rd, ttl);
}

void AddSrv(std::vector<uint8_t>* b, const std::string& name, uint16_t port,
            const std::string& target, uint32_t ttl = 120) {
  std::vector<uint8_t> rd;
  PutU16(&rd, 0);                                  // priority
  PutU16(&rd, 0);                                  // weight
  PutU16(&rd, port);
  PutName(&rd, target);
  PutRecord(b, name, 33, rd, ttl);
}

void AddTxt(std::vector<uint8_t>* b, const std::string& name,
            const std::vector<std::string>& kv, uint32_t ttl = 4500) {
  std::vector<uint8_t> rd;
  for (size_t i = 0; i < kv.size(); ++i) {
    rd.push_back(static_cast<uint8_t>(kv[i].size()));
    rd.insert(rd.end(), kv[i].begin(), kv[i].end());
  }
  rd.push_back(0);                                 // a lone NUL closes the set
  PutRecord(b, name, 16, rd, ttl);
}

void AddA(std::vector<uint8_t>* b, const std::string& name, const uint8_t ip[4],
          uint32_t ttl = 120) {
  std::vector<uint8_t> rd(ip, ip + 4);
  PutRecord(b, name, 1, rd, ttl);
}

// Read a name from a query, following compression pointers.  *off ends up just
// past the name as it appeared, not past wherever it pointed.
bool ReadName(const uint8_t* p, size_t n, size_t* off, std::string* out,
              int* hops) {
  size_t o = *off;
  bool jumped = false;
  out->clear();
  while (true) {
    if (o >= n)
      return false;
    const uint8_t len = p[o];
    if ((len & 0xc0) == 0xc0) {
      if (o + 1 >= n)
        return false;
      const size_t target = ((len & 0x3f) << 8) | p[o + 1];
      if (!jumped)
        *off = o + 2;
      jumped = true;
      if (--(*hops) <= 0)
        return false;
      o = target;
      continue;
    }
    if (len == 0) {
      ++o;
      break;
    }
    if (o + 1 + len > n)
      return false;
    if (!out->empty())
      *out += '.';
    out->append(reinterpret_cast<const char*>(p + o + 1), len);
    o += 1 + len;
  }
  if (!jumped)
    *off = o;
  return true;
}

// Every Pepper interface the rest of the project depends on.  Presence is
// reported; absence is a finding, not a crash.
const char* kInterfaces[] = {
    "PPB_Graphics2D;1.0",
    "PPB_Graphics2D;1.1",
    "PPB_ImageData;1.0",
    "PPB_TCPSocket;1.0",
    "PPB_TCPSocket;1.1",
    "PPB_TCPSocket;1.2",
    "PPB_TCPServerSocket_Private;0.1",
    "PPB_TCPServerSocket_Private;0.2",
    "PPB_UDPSocket;1.0",
    "PPB_UDPSocket;1.1",
    "PPB_UDPSocket;1.2",
    "PPB_UDPSocketExtension_Samsung;0.1",
    "PPB_NetAddress;1.0",
    "PPB_HostResolver;1.0",
    "PPB_MediaPlayer_Samsung;1.0",
    "PPB_MediaPlayer_Samsung;1.1",
    "PPB_MediaPlayer_Samsung;1.2",
    "PPB_MediaPlayer_Samsung;1.3",
    "PPB_MediaPlayer_Samsung;1.4",
    "PPB_MediaDataSource_Samsung;1.0",
    "PPB_URLDataSource_Samsung;1.0",
    "PPB_ESDataSource_Samsung;1.0",
    "PPB_CompositorLayer_Samsung;0.1",
    "PPB_VideoDecoder;1.0",
    "PPB_VideoDecoder;1.1",
    "PPB_Audio;1.0",
    "PPB_Audio;1.1",
    "PPB_Graphics3D;1.0",
    "PPB_Core;1.0",
    "PPB_MessageLoop;1.0",
    "PPB_Messaging;1.0",
    "PPB_FileIO;1.0",
};

}  // namespace

class AirPlayInstance : public pp::Instance {
 public:
  explicit AirPlayInstance(PP_Instance instance)
      : pp::Instance(instance), callback_factory_(this), video_(this), audio_(this), url_video_(this) {}

  virtual bool Init(uint32_t /*argc*/, const char* /*argn*/[],
                    const char* /*argv*/[]) {
    Report("DIAG nacl module Init() ran — M1 module loaded");
    ProbeInterfaces();
    StartTcp();
    StartUdp();
    StartMdns();        // advertise now, with the assumed address
    StartStreams();     // the ports SETUP will advertise
    StartNtp();
    StartAudioUdp();

    return true;
  }

  virtual void HandleMessage(const pp::Var& message) {
    if (!message.is_string())
      return;
    const std::string text = message.AsString();
    // The page attaches its message listener ~500 ms after load, by which time
    // Init() has already reported.  It asks for the report again from there.
    if(text.compare(0,8,"network|")==0) {
      size_t sep=text.find('|',8); if(sep==std::string::npos) return;
      std::string ip=text.substr(8,sep-8),mac=text.substr(sep+1);
      unsigned a,b,c,d,m[6]; char trailing;
      if(sscanf(ip.c_str(),"%u.%u.%u.%u%c",&a,&b,&c,&d,&trailing)!=4 || !a || a>223 || b>255 || c>255 || d>255) return;
      if(sscanf(mac.c_str(),"%x:%x:%x:%x:%x:%x%c",m,m+1,m+2,m+3,m+4,m+5,&trailing)!=6) return;
      for(int i=0;i<6;++i) if(m[i]>255) return;
      local_ip_[0]=a;local_ip_[1]=b;local_ip_[2]=c;local_ip_[3]=d;
      char full[18],flat[13]; snprintf(full,sizeof(full),"%02X:%02X:%02X:%02X:%02X:%02X",m[0],m[1],m[2],m[3],m[4],m[5]);
      snprintf(flat,sizeof(flat),"%02X%02X%02X%02X%02X%02X",m[0],m[1],m[2],m[3],m[4],m[5]);
      kDeviceId=full;kRaopMac=flat;kHostName="LanternCast-"+std::string(flat)+".local";
      Report("DIAG TV network identity configured"); Announce("network configured");return;
    }
    if (text == "capture-next-frame") { capture_frame_=true; return; }
    if (text == "capture-next-audio") { capture_audio_=true; return; }
    if (text == "report") {
      Report("DIAG nacl re-report requested by the page");
      ProbeInterfaces();
      ReportState();
      return;
    }
    Report("DIAG nacl echo: " + text);
  }

  virtual void DidChangeView(const pp::View& view) {
    pp::Rect rect = view.GetRect();
    if (!video_.started()) StartPaint(rect.width(), rect.height());
  }

 private:
  void Report(const std::string& text) { PostMessage(pp::Var(text)); }

  static const char* B(bool v) { return v ? "true" : "false"; }

  // Re-emitted on request: what the sockets actually achieved.
  void ReportState() {
    std::ostringstream oss;
    oss << "DIAG SUMMARY tcp_bound=" << B(tcp_bound_)
        << " tcp_listening=" << B(tcp_listening_)
        << " tcp_accepted=" << B(tcp_accepted_)
        << " udp_bound=" << B(udp_bound_)
        << " udp_joined=" << B(udp_joined_)
        << " udp_recv_ok=" << B(udp_recv_ok_)
        << " mdns_bound=" << B(mdns_bound_)
        << " mdns_joined=" << B(mdns_joined_)
        << " mdns_queries=" << mdns_queries_
        << " mdns_answered=" << mdns_answered_
        << " streams=" << B(stream_listening_[0]) << "/"
        << B(stream_listening_[1]) << " stream_conns=" << stream_conns_
        << " ntp_bound=" << B(ntp_bound_)
        << " ntp_packets=" << ntp_packets_
        << " setup_requests=" << setup_requests_
        << " setup_done=" << B(setup_done_);
    Report(oss.str());
    // The listener banners are emitted in Init(), which runs before the page is
    // forwarding messages -- so they are re-stated here, where they survive.
    for (int i = 0; i < kStreamCount; ++i) {
      Report(std::string("DIAG ") + kStreamNames[i] + " listener port " +
             PortText(kStreamPorts[i]) +
             (stream_listening_[i] ? " LISTENING" : " NOT listening"));
    }
    if (udp_joined_)
      SendUdpPing();  // exercise the UDP path while the page is listening
    if (mdns_joined_)
      Announce("re-report");  // macOS may have missed the startup announcement
  }

  void ArmUdpRecv() {
    if (udp_recv_pending_)
      return;
    udp_recv_pending_ = true;
    udp_recv_buffer_.resize(1500);
    int32_t r = udp_socket_.RecvFrom(
        &udp_recv_buffer_[0], static_cast<int32_t>(udp_recv_buffer_.size()),
        callback_factory_.NewCallbackWithOutput(
            &AirPlayInstance::OnUdpRecvFrom));
    if (r != PP_OK_COMPLETIONPENDING) {
      udp_recv_pending_ = false;
      std::ostringstream oss;
      oss << "DIAG FAIL udp RecvFrom return=" << r << " (" << ResultName(r)
          << ")";
      Report(oss.str());
    }
  }

  // ---- M1: interface probe -------------------------------------------------

  void ProbeInterfaces() {
    pp::Module* module = pp::Module::Get();
    if (!module) {
      Report("DIAG FAIL pp::Module::Get() returned null");
      return;
    }
    for (size_t i = 0; i < sizeof(kInterfaces) / sizeof(kInterfaces[0]); ++i) {
      const void* iface = module->GetBrowserInterface(kInterfaces[i]);
      Report(std::string("DIAG iface ") + kInterfaces[i] + " = " +
             (iface ? "present" : "ABSENT"));
    }
  }

  // ---- M1: paint -----------------------------------------------------------

  void StartPaint(int width, int height) {
    if (width <= 0 || height <= 0) {
      Report("DIAG FAIL view rect is empty — nothing to paint");
      return;
    }
    if (graphics_2d_.is_null()) {
      graphics_2d_ = pp::Graphics2D(this, pp::Size(width, height), true);
      if (graphics_2d_.is_null() || !BindGraphics(graphics_2d_)) {
        Report("DIAG FAIL BindGraphics failed — M1 paint not possible");
        return;
      }
      Report("DIAG paint BindGraphics ok");
    }

    pp::ImageData image(this, PP_IMAGEDATAFORMAT_BGRA_PREMUL,
                        pp::Size(width, height), false);
    if (image.is_null()) {
      Report("DIAG FAIL ImageData allocation failed");
      return;
    }
    uint32_t* pixels = static_cast<uint32_t*>(image.data());
    int stride = image.stride() / 4;
    // BGRA_PREMUL on a little-endian host: 0xAARRGGBB.  Transparent: M1 proved
    // the module can own the screen and that proof is in the log now, so the
    // page's CSS owns every visible pixel.  This fill used to be solid green --
    // the "big green screen" was this buffer.
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x)
        pixels[y * stride + x] = 0x00000000u;
    }

    graphics_2d_.PaintImageData(image, pp::Point(0, 0));
    int32_t r = graphics_2d_.Flush(
        callback_factory_.NewCallback(&AirPlayInstance::OnFlushDone));
    std::ostringstream oss;
    oss << "DIAG paint " << width << "x" << height
        << " Flush return=" << r << " (" << ResultName(r) << ")";
    Report(oss.str());
  }

  void OnFlushDone(int32_t result) {
    std::ostringstream oss;
    oss << "DIAG paint Flush callback result=" << result << " ("
        << ResultName(result) << ") — M1 paint done";
    Report(oss.str());
  }

  // ---- M2a: TCP listening socket ------------------------------------------

  void StartTcp() {
    tcp_socket_ = pp::TCPSocket(this);
    uint8_t any[4] = {0, 0, 0, 0};
    pp::NetAddress addr = MakeIPv4(this, any, kTcpPort);
    int32_t r = tcp_socket_.Bind(
        addr, callback_factory_.NewCallback(&AirPlayInstance::OnTcpBind));
    std::ostringstream oss;
    oss << "DIAG tcp Bind 0.0.0.0:" << kTcpPort << " return=" << r << " ("
        << ResultName(r) << ")";
    Report(oss.str());
  }

  void OnTcpBind(int32_t result) {
    std::ostringstream oss;
    oss << "DIAG tcp Bind callback result=" << result << " ("
        << ResultName(result) << ")";
    Report(oss.str());
    if (result != PP_OK)
      return;
    tcp_bound_ = true;

    int32_t r = tcp_socket_.Listen(
        10, callback_factory_.NewCallback(&AirPlayInstance::OnTcpListen));
    std::ostringstream oss2;
    oss2 << "DIAG tcp Listen backlog=10 return=" << r << " ("
         << ResultName(r) << ")";
    Report(oss2.str());
  }

  void OnTcpListen(int32_t result) {
    {
      std::ostringstream oss;
      oss << "DIAG tcp Listen callback result=" << result << " ("
          << ResultName(result) << ")";
      Report(oss.str());
    }
    if (result != PP_OK)
      return;
    tcp_listening_ = true;

    std::string local = tcp_socket_.GetLocalAddress().DescribeAsString(true).AsString();
    Report("DIAG tcp LISTENING on " + local +
           " — M2a TCP half reached (verify from the Mac)");
    AcceptNext();
  }

  void AcceptNext() {
    int32_t r = tcp_socket_.Accept(
        callback_factory_.NewCallbackWithOutput(
            &AirPlayInstance::OnTcpAccept));
    if (r != PP_OK_COMPLETIONPENDING) {
      std::ostringstream oss;
      oss << "DIAG FAIL tcp Accept return=" << r << " (" << ResultName(r)
          << ")";
      Report(oss.str());
    }
  }

  void OnTcpAccept(int32_t result, pp::TCPSocket socket) {
    tcp_accepted_ = true;
    {
      std::ostringstream oss;
      oss << "DIAG tcp peer connected from "
          << socket.GetRemoteAddress().DescribeAsString(true).AsString()
          << " result=" << result;
      Report(oss.str());
    }

    // AirPlay holds more than one connection to :7000 at once.  The control
    // connection carries /info and SETUP; the client also dials the eventPort we
    // advertise -- which is this same port -- and holds a second socket open
    // beside it.  A single slot let the second peer replace the first, so the
    // control connection died mid-handshake and macOS reported "could not
    // connect" on a session that had just been answered correctly.  Each
    // accepted socket now keeps its own slot and its own buffers.
    int ci = -1;
    for (int i = 0; i < kMaxHttpConns; ++i)
      if (!http_conns_[i].in_use) { ci = i; break; }
    if (ci < 0) {
      Report("DIAG http every connection slot is busy — refusing the new peer");
      socket.Close();
      AcceptNext();
      return;
    }

    HttpConn& c = http_conns_[ci];
    c.in_use = true;
    c.socket = socket;
    c.request.clear();
    c.write_out.clear();
    c.read_pending = false;
    c.write_pending = false;
    {
      std::ostringstream oss;
      oss << "DIAG http slot " << ci << " open — " << OpenHttpConns() << " of "
          << kMaxHttpConns << " in use";
      Report(oss.str());
    }
    ArmTcpRead(ci);
    AcceptNext();
  }

  int OpenHttpConns() const {
    int n = 0;
    for (int i = 0; i < kMaxHttpConns; ++i)
      if (http_conns_[i].in_use) ++n;
    return n;
  }

  // Closing a slot must touch only that slot: the whole point of the pool is
  // that one peer going away leaves the others alone.
  void CloseHttpConn(int ci) {
    HttpConn& c = http_conns_[ci];
    if (!c.in_use) return;
    c.socket.Close();
    c.in_use = false;
    c.read_pending = false;
    c.write_pending = false;
    c.request.clear();
    c.write_out.clear();
    std::ostringstream oss;
    oss << "DIAG http slot " << ci << " closed — " << OpenHttpConns() << " of "
        << kMaxHttpConns << " in use";
    Report(oss.str());
  }

  // ---- M4: the AirPlay HTTP endpoint --------------------------------------
  //
  // The Mac's first move after discovering us is `GET /info` on :7000, and it
  // reports "could not connect" if the socket closes without any HTTP reply --
  // which is exactly what the M2 banner did.  So this is a real (if minimal)
  // HTTP/1.1 server: every request gets a status line, and every request is
  // logged with its path and headers so the handshake can be walked one
  // observable step at a time rather than guessed at.

  void ArmTcpRead(int ci) {
    HttpConn& c = http_conns_[ci];
    if (!c.in_use) return;
    c.read_buffer.resize(8192);
    int32_t r = c.socket.Read(&c.read_buffer[0],
                              static_cast<int32_t>(c.read_buffer.size()),
                              callback_factory_.NewCallback(
                                  &AirPlayInstance::OnTcpRead, ci));
    if (r != PP_OK_COMPLETIONPENDING) {
      std::ostringstream oss;
      oss << "DIAG FAIL tcp Read return=" << r << " (" << ResultName(r)
          << ") on slot " << ci;
      Report(oss.str());
      CloseHttpConn(ci);
    }
  }

  void OnTcpRead(int32_t result, int ci) {
    HttpConn& c = http_conns_[ci];
    if (!c.in_use) return;
    if (result <= 0) {
      std::ostringstream oss;
      oss << "DIAG http connection closed (read=" << result << ", slot " << ci
          << ")";
      Report(oss.str());
      CloseHttpConn(ci);
      return;
    }
    c.request.append(&c.read_buffer[0], static_cast<size_t>(result));

    bool handled = false;
    while (!handled) {
      size_t head = c.request.find("\r\n\r\n");
      if (head == std::string::npos) break;
      size_t body_start = head + 4;
      size_t want = ContentLength(c.request);
      if (c.request.size() < body_start + want) break;
      std::string message = c.request.substr(0, body_start + want);
      c.request.erase(0, body_start + want);
      // Handlers reply synchronously, so they address the connection through
      // this one field rather than through an argument on every one of them.
      cur_ = ci;
      HandleRequest(message);
      cur_ = -1;
      handled = true;  // the write completes before the next read is armed
    }

    if (c.request.size() > 256 * 1024) {
      Report("DIAG http request over 256 KB with no complete header — closing");
      CloseHttpConn(ci);
      return;
    }
    // A write is outstanding, so the read resumes from OnTcpWrite: this stack
    // dislikes two pending operations on one socket.
    if (handled) {
      if (c.write_pending) {
        c.read_pending = true;
      } else {
        // Nothing went out, so nothing will call back to re-arm the read.
        ArmTcpRead(ci);
      }
      return;
    }
    ArmTcpRead(ci);
  }

  static size_t ContentLength(const std::string& msg) {
    size_t head = msg.find("\r\n\r\n");
    if (head == std::string::npos) return 0;
    std::string lower = msg.substr(0, head);
    for (size_t i = 0; i < lower.size(); ++i)
      if (lower[i] >= 'A' && lower[i] <= 'Z') lower[i] += 'a' - 'A';
    size_t p = lower.find("content-length:");
    if (p == std::string::npos) return 0;
    p += 15;
    while (p < lower.size() && lower[p] == ' ') ++p;
    size_t n = 0;
    while (p < lower.size() && lower[p] >= '0' && lower[p] <= '9')
      n = n * 10 + static_cast<size_t>(lower[p++] - '0');
    return n;
  }

  static char LowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  }

  // Header names are case-insensitive, and `cseq` can also occur inside a
  // binary body, so the search stops at the blank line.  `name` must be
  // lowercase.
  static std::string Header(const std::string& msg, const char* name) {
    size_t head_end = msg.find("\r\n\r\n");
    std::string head = msg.substr(0, head_end == std::string::npos
                                         ? msg.size() : head_end);
    std::string lower = head;
    for (size_t i = 0; i < lower.size(); ++i) lower[i] = LowerAscii(lower[i]);

    std::string needle = std::string("\r\n") + name + ":";
    size_t at = lower.find(needle);
    if (at == std::string::npos) {
      // A header on the very first line cannot happen in a request, but a
      // response echoed back for diagnosis can, so check the beginning too.
      needle = std::string(name) + ":";
      if (lower.compare(0, needle.size(), needle) != 0) return std::string();
      at = 0;
    }
    size_t v = at + needle.size();
    while (v < head.size() && (head[v] == ' ' || head[v] == '\t')) ++v;
    size_t e = head.find("\r\n", v);
    return head.substr(v, (e == std::string::npos ? head.size() : e) - v);
  }

  void HandleRequest(const std::string& msg) {
    size_t line_end = msg.find("\r\n");
    std::string line = (line_end == std::string::npos) ? msg : msg.substr(0, line_end);
    size_t sp1 = line.find(' ');
    size_t sp2 = (sp1 == std::string::npos) ? std::string::npos
                                            : line.find(' ', sp1 + 1);
    std::string method = (sp1 == std::string::npos) ? line : line.substr(0, sp1);
    std::string path = (sp2 == std::string::npos)
                           ? "/"
                           : line.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string proto = (sp2 == std::string::npos) ? "" : line.substr(sp2 + 1);

    // Answer in the client's own protocol and with its own CSeq.  curl sends
    // HTTP/1.1, the Mac sends RTSP/1.0, and both have to get a reply they
    // recognise.
    rtsp_ = (proto.compare(0, 8, "RTSP/1.0") == 0);
    cseq_ = Header(msg, "cseq");

    ++http_requests_;
    {
      std::ostringstream oss;
      oss << "DIAG http #" << http_requests_ << " " << method << " " << path
          << " — " << msg.size() << " bytes";
      Report(oss.str());
    }
    // The headers carry the protocol version and the Mac's expectations; this
    // is the line that says what the next milestone has to implement.
    {
      size_t head = msg.find("\r\n\r\n");
      std::string h = msg.substr(0, head == std::string::npos ? msg.size() : head);
      for (size_t i = 0; i + 1 < h.size(); ++i)
        if (h[i] == '\r' && h[i + 1] == '\n') { h.replace(i, 2, " | "); }
      if (h.size() > 360) h = h.substr(0, 360) + "…";
      Report("DIAG http hdr: " + h);
    }
    size_t body_at = msg.find("\r\n\r\n");
    // Held outside the log block: /fp-setup routes on the body's length, so
    // the bytes are not just diagnostics any more.
    const std::string body =
        (body_at == std::string::npos) ? std::string() : msg.substr(body_at + 4);
    if (!body.empty()) {
      // The whole body, 48 bytes to a line.  There is no cap here on purpose:
      // the report server writes what it is given, so anything missing from the
      // log was missing from this loop -- and a body cut off at 48 bytes is
      // exactly where the SETUP walk stopped being diagnosable.
      for (size_t i = 0; i < body.size(); i += 48) {
        std::ostringstream oss;
        oss << "DIAG http body[" << body.size() << "] @" << i;
        for (size_t j = i; j < body.size() && j < i + 48; ++j) {
          char b[8];
          snprintf(b, sizeof(b), " %02x", static_cast<unsigned char>(body[j]));
          oss << b;
        }
        Report(oss.str());
      }
    }

    const std::string route=path.substr(0,path.find('?'));
    const std::string peer=http_conns_[cur_].socket.GetRemoteAddress().DescribeAsString(false).AsString();
    if((path=="/fp-setup" || path=="/play" || method=="SETUP") && (mirror_ready_ || audio_active_ || url_video_.active()) && !session_peer_.empty() && peer!=session_peer_)
      return Respond(453,"text/plain","receiver already in use\n");
    if(path=="/fp-setup" || path=="/play")session_peer_=peer;
    if(method=="HTTP/1.1" || method=="HTTP/1.0") return;
    if(route=="/reverse" && method=="POST") {
      Respond(101,"","","Connection: Upgrade\r\nUpgrade: PTTH/1.0\r\n");
    } else if(route=="/play" && method=="POST") {
      Bplist* play=Bplist::Parse(body.data(),body.size());
      std::string url=Header(body,"content-location"),cookie,agent;
      double start=0;bool fraction=true;
      if(play) {
        const Bplist* location=play->get("Content-Location");
        if(location && location->Is(Bplist::kString))url=location->str();
        const Bplist* pos=play->get("Start-Position-Seconds");
        if(pos)fraction=false;else pos=play->get("Start-Position");
        if(pos)start=pos->Is(Bplist::kReal)?pos->real():pos->Is(Bplist::kUint)?pos->uint():0;
        const Bplist* headers=play->get("HTTPHeaders");
        const Bplist* c=headers?headers->get("Cookie"):NULL;
        const Bplist* a=headers?headers->get("User-Agent"):NULL;
        if(c && c->Is(Bplist::kString))cookie=c->str();
        if(a && a->Is(Bplist::kString))agent=a->str();
      } else {std::string pos=Header(body,"start-position");if(!pos.empty())start=strtod(pos.c_str(),NULL);}
      delete play;
      if((url.compare(0,7,"http://")!=0 && url.compare(0,8,"https://")!=0) || !std::isfinite(start) || start<0)
        return Respond(400,"text/plain","unsupported video location\n");
      video_.Reset();audio_.Reset();audio_active_=false;
      url_video_.Start(url,start,fraction,cookie,agent);
      Respond(200,"","");
    } else if(route=="/playback-info" && method=="GET") {
      Respond(200,"text/x-apple-plist+xml",url_video_.Info());
    } else if(route=="/scrub") {
      if(method=="GET") Respond(200,"text/parameters",url_video_.Scrub());
      else {size_t p=path.find("position=");if(p==std::string::npos)return Respond(400,"","");url_video_.Seek(strtod(path.c_str()+p+9,NULL));Respond(200,"","");}
    } else if(route=="/rate" && method=="POST") {
      size_t p=path.find("value=");if(p==std::string::npos)return Respond(400,"","");url_video_.Rate(strtod(path.c_str()+p+6,NULL));Respond(200,"","");
    } else if(route=="/stop" && method=="POST") {
      url_video_.Reset();video_epoch_=-1;Report("DIAG session stopped");Respond(200,"","");
    } else if (method == "OPTIONS") {
      Respond(200, "text/plain", "");
    } else if (path == "/info" || path == "/server-info") {
      Respond(200, "application/x-apple-binary-plist", InfoPlist());
    } else if (path == "/pair-pin-start") {
      Respond(200, "text/x-apple-plist+xml",
              "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              "<plist version=\"1.0\"><dict/></plist>\n");
    } else if (path == "/fp-setup") {
      FpSetup(body);
    } else if (method == "SETUP") {
      Setup(body);
    } else if (method == "GET_PARAMETER") {
      Respond(200, "text/parameters", body.find("volume") != std::string::npos ? "volume: 0.0\r\n" : "");
    } else if (path == "/feedback") {
      Respond(200, "text/plain", "");
    } else if (path == "/audioMode" && method == "POST") {
      Bplist* mode=Bplist::Parse(body.data(),body.size());
      if(!mode || !mode->Is(Bplist::kDict)) {delete mode;return Respond(400,"text/plain","invalid audio mode\n");}
      Report("DIAG audio mode acknowledged "+NodeText(mode->get("audioMode")));
      delete mode;Respond(200,"text/plain", "");
    } else if (method == "TEARDOWN") {
      bool stop_video=body.empty(),stop_audio=body.empty();
      Bplist* teardown=Bplist::Parse(body.data(),body.size());
      const Bplist* streams=teardown?teardown->get("streams"):NULL;
      if(!streams) stop_video=stop_audio=true;
      if(streams && streams->Is(Bplist::kArray)) for(size_t i=0;i<streams->count();++i) {
        const Bplist* item=streams->item(i);const Bplist* type=item?item->get("type"):NULL;
        if(type && type->Is(Bplist::kUint)) {if(type->uint()==110) stop_video=true;if(type->uint()==96) stop_audio=true;}
      }
      delete teardown;
      if(stop_video) {video_.Reset();mirror_ready_=false;Report("DIAG session stopped");}
      if(stop_audio) {audio_.Reset();audio_active_=false;Report("DIAG audio session stopped");}
      Respond(200,"text/plain", "");
    } else if (method == "RECORD" || method == "FLUSH" ||
               method == "TEARDOWN" || method == "SET_PARAMETER" ||
               method == "GET_PARAMETER") {
      // Acknowledged rather than 404'd.  None of these carries anything this
      // build acts on yet, but refusing one is what makes a client abandon a
      // session that is otherwise ready to stream.
      Report("DIAG " + method + " " + path + " -> 200 (ack)");
      Respond(200, "text/plain", "");
    } else {
      // Not implemented yet.  Answering honestly rather than dropping the
      // socket is the point: the log then names the exact path the Mac wanted.
      Report("DIAG M4 TODO unimplemented AirPlay path: " + method + " " + path);
      Respond(404, "text/plain", "not implemented\n");
    }
  }

  // What a classic AirPlay receiver answers `GET /info` with.  The values match
  // the mDNS TXT records, because a client that sees one set at discovery and
  // another here will refuse the device.
  std::string InfoPlist() {
    BplistBuilder b;
    typedef std::pair<std::string, int> P;
    std::vector<P> fields;
    fields.push_back(P("deviceID", b.Str(kDeviceId)));
    fields.push_back(P("macAddress", b.Str(kDeviceId)));
    fields.push_back(P("features", b.Uint(0x1E527FFFF7ULL)));
    fields.push_back(P("statusFlags", b.Uint(68)));
    fields.push_back(P("model", b.Str(kModel)));
    fields.push_back(P("name", b.Str(kServiceName)));
    fields.push_back(P("pi", b.Str(kPairingId)));
    fields.push_back(P("sourceVersion", b.Str(kSrcVersion)));
    fields.push_back(P("vv", b.Uint(2)));
    fields.push_back(P("keepAliveLowPower", b.Uint(1)));
    fields.push_back(P("keepAliveSendStatsAsBody", b.Uint(1)));
    std::vector<P> display;
    display.push_back(P("uuid", b.Str(kPairingId)));
    display.push_back(P("width", b.Uint(1920)));
    display.push_back(P("height", b.Uint(1080)));
    display.push_back(P("widthPixels", b.Uint(1920)));
    display.push_back(P("heightPixels", b.Uint(1080)));
    display.push_back(P("widthPhysical", b.Uint(0)));
    display.push_back(P("heightPhysical", b.Uint(0)));
    display.push_back(P("rotation", b.Uint(0)));
    display.push_back(P("refreshRate", b.Uint(60)));
    display.push_back(P("maxFPS", b.Uint(30)));
    display.push_back(P("overscanned", b.Uint(0)));
    display.push_back(P("features", b.Uint(14)));
    std::vector<int> displays(1, b.Dict(display));
    fields.push_back(P("displays", b.Array(displays)));
    std::vector<int> formats, latencies;
    for (int t = 100; t <= 101; ++t) {
      std::vector<P> fmt;
      fmt.push_back(P("type", b.Uint(t)));
      fmt.push_back(P("audioInputFormats", b.Uint(0x3fffffc)));
      fmt.push_back(P("audioOutputFormats", b.Uint(0x3fffffc)));
      formats.push_back(b.Dict(fmt));
      std::vector<P> lat;
      lat.push_back(P("type", b.Uint(t)));
      lat.push_back(P("audioType", b.Str("default")));
      lat.push_back(P("inputLatencyMicros", b.Uint(0)));
      lat.push_back(P("outputLatencyMicros", b.Uint(0)));
      latencies.push_back(b.Dict(lat));
    }
    fields.push_back(P("audioFormats", b.Array(formats)));
    fields.push_back(P("audioLatencies", b.Array(latencies)));
    return b.Encode(b.Dict(fields));
  }

  // `POST /fp-setup` — the legacy FairPlay handshake.  Apple sends exactly two
  // shapes and tells them apart by body length alone, so that is the test:
  //
  //    16 bytes  -> 142 bytes: one of four canned blobs, picked by body[14].
  //                             A constant-table lookup; no crypto at all.
  //    164 bytes ->  32 bytes: a 12-byte header plus the request's last 20
  //                            bytes echoed straight back.  The request itself
  //                            is kept, because SETUP's ekey is decrypted
  //                            against it.
  //
  // Both replies are binary and both must carry Content-Type
  // application/octet-stream, which is what the client checks.
  void FpSetup(const std::string& body) {
    const unsigned char* in =
        reinterpret_cast<const unsigned char*>(body.data());

    if (body.size() == 16) {
      std::string out(142, '\0');
      unsigned char mode = in[14];
      if (fairplay_setup(&fairplay_, in,
                         reinterpret_cast<unsigned char*>(&out[0])) != 0) {
        std::ostringstream oss;
        oss << "DIAG FAIL fp-setup stage 1 rejected: version=" << int(in[4])
            << " mode=" << int(mode);
        Report(oss.str());
        return Respond(400, "text/plain", "bad fp-setup\n");
      }
      std::ostringstream oss;
      oss << "DIAG fp-setup stage 1 ok: 16 -> 142, mode=" << int(mode);
      Report(oss.str());
      return Respond(200, "application/octet-stream", out);
    }

    if (body.size() == 164) {
      std::string out(32, '\0');
      if (fairplay_handshake(&fairplay_, in,
                             reinterpret_cast<unsigned char*>(&out[0])) != 0) {
        std::ostringstream oss;
        oss << "DIAG FAIL fp-setup stage 2 rejected: version=" << int(in[4]);
        Report(oss.str());
        return Respond(400, "text/plain", "bad fp-setup\n");
      }
      Report("DIAG fp-setup stage 2 ok: 164 -> 32, key message stored");
      return Respond(200, "application/octet-stream", out);
    }

    {
      std::ostringstream oss;
      oss << "DIAG FAIL fp-setup body length " << body.size()
          << " is neither 16 nor 164";
      Report(oss.str());
    }
    Respond(400, "text/plain", "bad fp-setup\n");
  }

  // `SETUP` — the request that names the streams and hands over the key.
  //
  // The body is a binary plist and the reply has to be one too.  Two things in
  // it matter: `streams`, whose elements say what kind of stream the client
  // wants and on which port it will listen for audio control, and `ekey` —
  // 72 bytes of FairPlay-encrypted AES key that yields the real 16 bytes only
  // when decrypted against the key message /fp-setup stored.
  void Setup(const std::string& body) {
    ++setup_requests_;

    Bplist* root = Bplist::Parse(body.data(), body.size());
    if (!root || !root->Is(Bplist::kDict)) {
      delete root;
      Report("DIAG FAIL SETUP body is not a binary plist dictionary");
      return Respond(400, "text/plain", "bad setup\n");
    }

    // A structural dump of the whole body, not only the fields we expect.  The
    // stream walk further down can come up empty four different ways and a
    // single failure line cannot tell them apart; this names every key, its
    // type, and the shape of `streams` itself, so one click is decisive.
    {
      std::ostringstream oss;
      oss << "DIAG setup root[" << root->count() << "]:";
      for (size_t i = 0; i < root->count(); ++i)
        oss << " " << root->key(i) << "=" << NodeText(root->value(i));
      Report(oss.str());
    }
    {
      const Bplist* s = root->get("streams");
      if (!s) {
        Report("DIAG setup: streams key is absent from the root dictionary");
      } else if (!s->Is(Bplist::kArray)) {
        Report("DIAG setup: streams is " + NodeText(s) + ", not an array");
      } else {
        Report("DIAG setup: streams array[" + IndexText(s->count()) + "]");
        for (size_t i = 0; i < s->count(); ++i) {
          const Bplist* e = s->item(i);
          if (e && e->Is(Bplist::kDict)) {
            std::ostringstream oss;
            oss << "DIAG setup streams[" << i << "] dict[" << e->count() << "]:";
            for (size_t k = 0; k < e->count(); ++k)
              oss << " " << e->key(k) << "=" << NodeText(e->value(k));
            Report(oss.str());
          } else {
            Report("DIAG setup streams[" + IndexText(i) + "] is " + NodeText(e) +
                   ", not a dictionary");
          }
        }
      }
    }

    // Logged rather than assumed.  RTP/AVP/TCP would mean the media arrives
    // over the RTSP connection itself; anything else needs UDP stream ports,
    // which this build does not open yet.
    Report("DIAG setup: Transport=" + NodeText(root->get("Transport")));

    // This legacy session supplies no pair-verify shared secret.  UxPlay's modern path re-hashes the
    // FairPlay key as sha512(aeskey || ecdh_secret), but that secret is only
    // written by pair-verify — and this device advertises no legacy pairing, so
    // the client goes straight from /fp-setup to SETUP and has no secret to mix
    // in. Raw FairPlay key plus SHA512 mirroring derivation is verified live.
    const Bplist* ekey = root->get("ekey");
    if (ekey && (!ekey->Is(Bplist::kData) || ekey->str().size() != 72)) {
      Report("DIAG FAIL SETUP ekey is " + NodeText(ekey) + ", expected 72 bytes");
      delete root;
      return Respond(400, "text/plain", "bad setup\n");
    }
    if (ekey) {
    std::string key(16, '\0');
    if (fairplay_decrypt(&fairplay_,
                         reinterpret_cast<const unsigned char*>(ekey->str().data()),
                         reinterpret_cast<unsigned char*>(&key[0])) != 0) {
      delete root;
      Report("DIAG FAIL SETUP fairplay_decrypt rejected the ekey");
      return Respond(400, "text/plain", "bad setup\n");
    }
    memcpy(aes_key_, key.data(), 16);
    const Bplist* eiv = root->get("eiv");
    if (eiv && eiv->Is(Bplist::kData) && eiv->str().size() == 16)
      memcpy(aes_iv_, eiv->str().data(), 16);
    setup_done_ = true;
    const Bplist* tp = root->get("timingPort");
    if (tp && tp->Is(Bplist::kUint)) {
      PP_NetAddress_IPv4 peer;
      if (http_conns_[cur_].socket.GetRemoteAddress().DescribeAsIPv4Address(&peer)) {
        peer.port = Htons(tp->uint());
        timing_peer_ = pp::NetAddress(this, peer);
        if (!timing_active_) { timing_active_ = true; SendTiming(0); }
      }
    }
    Report("DIAG setup: legacy FairPlay session keys loaded");

    } else if (!setup_done_) {
      delete root;
      return Respond(400, "text/plain", "session keys missing\n");
    }

    BplistBuilder reply;
    std::vector<int> out_streams;
    const Bplist* streams = root->get("streams");
    if (streams && streams->Is(Bplist::kArray)) {
      for (size_t i = 0; i < streams->count(); ++i) {
        const Bplist* s = streams->item(i);
        if (!s || !s->Is(Bplist::kDict)) {
          Report("DIAG setup stream[" + IndexText(i) + "] skipped: " +
                 NodeText(s) + " is not a dictionary");
          continue;
        }
        const Bplist* type = s->get("type");
        const uint64_t t = (type && type->Is(Bplist::kUint)) ? type->uint() : 0;

        std::vector<std::pair<std::string, int> > entry;
        if (t == 110) {
          Report("DIAG setup stream[" + IndexText(i) + "] mirroring, connID=" +
                 NodeText(s->get("streamConnectionID")) + " -> dataPort " +
                 PortText(kVideoPort));
          const Bplist* id = s->get("streamConnectionID");
          if (id && id->Is(Bplist::kUint)) InitMirror(id->uint());
          entry.push_back(std::make_pair(std::string("type"), reply.Uint(110)));
          entry.push_back(std::make_pair(std::string("dataPort"),
                                         reply.Uint(kVideoPort)));
        } else if (t == 96) {
          const Bplist* cp = s->get("controlPort");

          Report("DIAG setup stream[" + IndexText(i) + "] audio, ct=" +
                 NodeText(s->get("ct")) + " controlPort=" + NodeText(cp) +
                 " -> dataPort " + PortText(kAudioPort));
          const Bplist* ct=s->get("ct");
          int codec=ct && ct->Is(Bplist::kUint)?ct->uint():8;
          if(codec!=4 && codec!=8) {delete root;return Respond(461,"text/plain","unsupported audio codec\n");}
          audio_.Start(codec); audio_active_=true; audio_epoch_set_=false; audio_packets_=0;
          audio_seq_set_=false;audio_duplicates_=0;audio_queue_.Reset();audio_sync_=false;audio_have_stamp_=false;
          entry.push_back(std::make_pair(std::string("type"), reply.Uint(96)));
          // Advertise our UDP control port, independently of the sender port.
          entry.push_back(std::make_pair(std::string("controlPort"),
                                         reply.Uint(7102)));
          entry.push_back(std::make_pair(std::string("dataPort"),
                                         reply.Uint(kAudioPort)));
        } else {
          delete root;
          Report("DIAG FAIL SETUP stream type " + IndexText(t) + " is unknown");
          return Respond(400, "text/plain", "bad setup\n");
        }
        out_streams.push_back(reply.Dict(entry));
      }
    }

    // UxPlay answers 200 whether or not the client named a stream it
    // recognises: raop_handlers.h wraps the whole streams block in
    // `if (PLIST_IS_ARRAY(req_streams_node))` and adds "streams" to the reply
    // only inside it.  Refusing here is what turned a client that said nothing
    // about streams into "could not connect".
    if (out_streams.empty())
      Report("DIAG setup: no usable stream named; replying 200 without streams, "
             "as UxPlay does");

    std::vector<std::pair<std::string, int> > top;
    if (!out_streams.empty())
      top.push_back(std::make_pair(std::string("streams"),
                                   reply.Array(out_streams)));
    top.push_back(std::make_pair(std::string("timingPort"),
                                 reply.Uint(kNtpPort)));
    // The event channel rides the RTSP connection the client already has.
    top.push_back(std::make_pair(std::string("eventPort"),
                                 reply.Uint(kTcpPort)));
    top.push_back(std::make_pair(std::string("timingProtocol"),
                                 reply.Str("NTP")));
    top.push_back(std::make_pair(std::string("receiverMAC"),
                                 reply.Data(MacBytes())));
    top.push_back(std::make_pair(std::string("name"),
                                 reply.Str(kServiceName)));
    const std::string out = reply.Encode(reply.Dict(top));
    delete root;

    {
      std::ostringstream oss;
      oss << "DIAG setup: replied " << out.size() << " bytes, "
          << out_streams.size() << " stream(s), timingPort=" << kNtpPort;
      Report(oss.str());
    }
    Respond(200, "application/x-apple-binary-plist", out);
  }

  // kRaopMac decoded to its six raw bytes, which is the Data form UxPlay puts in
  // receiverMAC.  The bytes, not their printable rendering: the client is told
  // the address, not its spelling.
  std::string MacBytes() const {
    std::string out;
    for (int i = 0; i < 6; ++i) {
      out += static_cast<char>((HexNibble(kRaopMac[2 * i]) << 4) |
                               HexNibble(kRaopMac[2 * i + 1]));
    }
    return out;
  }

  std::string IndexText(uint64_t v) const {
    std::ostringstream oss;
    oss << v;
    return oss.str();
  }

  std::string PortText(uint16_t p) const {
    std::ostringstream oss;
    oss << p;
    return oss.str();
  }

  void Respond(int code, const std::string& type, const std::string& body, const std::string& extra="") {
    if (cur_ < 0 || !http_conns_[cur_].in_use) {
      Report("DIAG FAIL reply with no connection in flight — dropped");
      return;
    }
    HttpConn& c = http_conns_[cur_];
    std::ostringstream head;
    head << (rtsp_ ? "RTSP/1.0 " : "HTTP/1.1 ") << code
         << (code == 200 ? " OK" : code == 101 ? " Switching Protocols" : code == 404 ? " Not Found" : " Error")
         << "\r\n";
    if (!cseq_.empty()) head << "CSeq: " << cseq_ << "\r\n";
    if (rtsp_) head << "Server: AirTunes/" << kSrcVersion << "\r\n";
    if (!type.empty()) head << "Content-Type: " << type << "\r\n";
    head << extra;
    head << "Content-Length: " << body.size() << "\r\n\r\n";
    // Write is asynchronous, so the bytes must outlive this call.
    c.write_out = head.str() + body;
    c.write_pending = false;
    int32_t r = c.socket.Write(c.write_out.data(),
                               static_cast<int32_t>(c.write_out.size()),
                               callback_factory_.NewCallback(
                                   &AirPlayInstance::OnTcpWrite, cur_));
    if (r == PP_OK_COMPLETIONPENDING) {
      c.write_pending = true;
    } else {
      std::ostringstream oss;
      oss << "DIAG FAIL tcp Write return=" << r << " (" << ResultName(r)
          << ") on slot " << cur_;
      Report(oss.str());
    }
  }

  void OnTcpWrite(int32_t result, int ci) {
    HttpConn& c = http_conns_[ci];
    if (!c.in_use) return;
    c.write_pending = false;
    std::ostringstream oss;
    oss << "DIAG http replied (" << c.write_out.size() << " bytes, write="
        << result << ", slot " << ci << ")";
    Report(oss.str());
    if (c.read_pending) {
      c.read_pending = false;
      ArmTcpRead(ci);
    }
  }

  // ---- M5 entry: the stream and timing ports SETUP advertises -------------
  //
  // A mirror data port is a TCP *listener*: the client dials in, it does not
  // wait to be called.  So these sockets have to be up before the SETUP reply
  // that names them, which is why they are bound once at startup on fixed ports
  // rather than opened in response to the request.  Accepting here and logging
  // the first bytes is what turns "did the picture stream arrive" into something
  // the log answers.

  void StartStreams() {
    for (int i = 0; i < kStreamCount; ++i) {
      stream_listen_[i] = pp::TCPSocket(this);
      uint8_t any[4] = {0, 0, 0, 0};
      pp::NetAddress addr = MakeIPv4(this, any, kStreamPorts[i]);
      int32_t r = stream_listen_[i].Bind(
          addr, callback_factory_.NewCallback(&AirPlayInstance::OnStreamBind,
                                              i));
      std::ostringstream oss;
      oss << "DIAG " << kStreamNames[i] << " Bind 0.0.0.0:" << kStreamPorts[i]
          << " return=" << r << " (" << ResultName(r) << ")";
      Report(oss.str());
    }
  }

  void OnStreamBind(int32_t result, int which) {
    {
      std::ostringstream oss;
      oss << "DIAG " << kStreamNames[which] << " Bind callback result="
          << result << " (" << ResultName(result) << ")";
      Report(oss.str());
    }
    if (result != PP_OK) return;
    int32_t r = stream_listen_[which].Listen(
        4, callback_factory_.NewCallback(&AirPlayInstance::OnStreamListen,
                                         which));
    if (r != PP_OK_COMPLETIONPENDING) {
      std::ostringstream oss;
      oss << "DIAG FAIL " << kStreamNames[which] << " Listen return=" << r
          << " (" << ResultName(r) << ")";
      Report(oss.str());
    }
  }

  void OnStreamListen(int32_t result, int which) {
    if (result != PP_OK) {
      std::ostringstream oss;
      oss << "DIAG FAIL " << kStreamNames[which] << " Listen callback result="
          << result << " (" << ResultName(result) << ")";
      Report(oss.str());
      return;
    }
    stream_listening_[which] = true;
    std::ostringstream oss;
    oss << "DIAG " << kStreamNames[which] << " LISTENING on "
        << stream_listen_[which].GetLocalAddress().DescribeAsString(true).AsString();
    Report(oss.str());
    AcceptStream(which);
  }

  void AcceptStream(int which) {
    int32_t r = stream_listen_[which].Accept(
        callback_factory_.NewCallbackWithOutput(
            &AirPlayInstance::OnStreamAccept, which));
    if (r != PP_OK_COMPLETIONPENDING) {
      std::ostringstream oss;
      oss << "DIAG FAIL " << kStreamNames[which] << " Accept return=" << r
          << " (" << ResultName(r) << ")";
      Report(oss.str());
    }
  }

  void OnStreamAccept(int32_t result, pp::TCPSocket socket, int which) {
    ++stream_conns_;
    {
      std::ostringstream oss;
      oss << "DIAG " << kStreamNames[which] << " peer connected from "
          << socket.GetRemoteAddress().DescribeAsString(true).AsString()
          << " result=" << result << " — stream " << stream_conns_;
      Report(oss.str());
    }
    stream_conn_[which] = socket;
    ArmStreamRead(which);
    AcceptStream(which);
  }

  void ArmStreamRead(int which) {
    stream_buf_[which].resize(4096);
    int32_t r = stream_conn_[which].Read(
        &stream_buf_[which][0], static_cast<int32_t>(stream_buf_[which].size()),
        callback_factory_.NewCallback(&AirPlayInstance::OnStreamRead, which));
    if (r != PP_OK_COMPLETIONPENDING) {
      std::ostringstream oss;
      oss << "DIAG FAIL " << kStreamNames[which] << " Read return=" << r << " ("
          << ResultName(r) << ")";
      Report(oss.str());
    }
  }

  void OnStreamRead(int32_t result, int which) {
    if (result <= 0) {
      std::ostringstream oss;
      oss << "DIAG " << kStreamNames[which] << " stream closed (read=" << result
          << ")";
      Report(oss.str());
      stream_conn_[which].Close();
      return;
    }
    const std::string chunk(&stream_buf_[which][0], static_cast<size_t>(result));
    if (which == 0) { ProcessMirror(chunk); ArmStreamRead(which); return; }
    std::ostringstream oss;
    oss << "DIAG " << kStreamNames[which] << " read " << result
        << " bytes: " << HexBytes(chunk, 16);
    Report(oss.str());
    ArmStreamRead(which);
  }

  void InitMirror(uint64_t id) {
    url_video_.Reset();
    unsigned char k[64], iv[64];
    std::string key(reinterpret_cast<const char*>(aes_key_),16);
    std::ostringstream num; num << id;
    Hash("AirPlayStreamKey"+num.str()+key,k);
    Hash("AirPlayStreamIV"+num.str()+key,iv);
    AES_init_ctx_iv(&mirror_aes_,k,iv);
    Report("DIAG mirror SHA512/AES-CTR configured");
    mirror_pos_=16; mirror_ready_=true; video_epoch_=-1; mirror_pending_.clear();
    Report("DIAG mirror AES ready connID="+num.str());
    video_.Reset();
    mirror_packets_=bad_frames_=0;mirror_codec_.clear();
    video_.Start(1920,1080);
  }
  static void Hash(const std::string& input, unsigned char output[64]) {
    crypto_hash(output,reinterpret_cast<const unsigned char*>(input.data()),input.size());
  }
  void DecryptMirror(std::string* payload) {
    for(size_t i=0;i<payload->size();++i) {
      if(mirror_pos_==16) {
        memcpy(mirror_block_,mirror_aes_.Iv,16);
        AES_ECB_encrypt(&mirror_aes_,mirror_block_);
        for(int j=15;j>=0;--j) if(++mirror_aes_.Iv[j]) break;
        mirror_pos_=0;
      }
      (*payload)[i]^=mirror_block_[mirror_pos_++];
    }
  }
  void ProcessMirror(const std::string& chunk) {
    mirror_pending_+=chunk;
    while(mirror_pending_.size()>=128) {
      const unsigned char* h=reinterpret_cast<const unsigned char*>(mirror_pending_.data());
      uint32_t n=static_cast<uint32_t>(h[0]) | (static_cast<uint32_t>(h[1])<<8) | (static_cast<uint32_t>(h[2])<<16) | (static_cast<uint32_t>(h[3])<<24);
      if(n>8*1024*1024) { Report("DIAG FAIL mirror packet exceeds limit"); mirror_pending_.clear(); return; }
      if(mirror_pending_.size()<128+n) return;
      int type=h[4];
      uint64_t stamp=0; for(int i=7;i>=0;--i) stamp=(stamp<<8)|h[8+i];
      double timestamp=static_cast<double>(stamp>>32)+static_cast<double>(stamp&0xffffffffULL)/4294967296.0;
      float width,height; memcpy(&width,h+40,4); memcpy(&height,h+44,4);
      std::string payload=mirror_pending_.substr(128,n);
      mirror_pending_.erase(0,128+n);
      if(type==1 && n>=11) {
        const unsigned char* p=reinterpret_cast<const unsigned char*>(payload.data());
        size_t ns=(p[6]<<8)|p[7];
        if(ns+11>n) continue;
        size_t np=(p[ns+9]<<8)|p[ns+10];
        if(ns+11+np>n) continue;
        mirror_config_=std::string("\0\0\0\1",4)+payload.substr(8,ns)+std::string("\0\0\0\1",4)+payload.substr(ns+11,np);
        mirror_codec_=mirror_config_;
        if(!url_video_.active()) {
          if(!video_.started())video_.Start(1920,1080);
          video_.Config(mirror_config_,std::isfinite(width)?static_cast<int>(width):0,std::isfinite(height)?static_cast<int>(height):0);
        }
        Report("DIAG mirror SPS/PPS received bytes="+IndexText(n)+" dimensions="+IndexText(width)+"x"+IndexText(height));
      } else if(type==0 && mirror_ready_) {
        if (mirror_packets_++==0) Report("DIAG mirror first ciphertext "+HexBytes(payload,64));
        DecryptMirror(&payload);
        size_t at=0; bool valid=true;
        while(at+4<payload.size()) {
          const unsigned char* p=reinterpret_cast<const unsigned char*>(payload.data()+at);
          uint32_t size=(static_cast<uint32_t>(p[0])<<24)|(static_cast<uint32_t>(p[1])<<16)|(static_cast<uint32_t>(p[2])<<8)|p[3];
          if(!size || at+4+size>payload.size() || (p[4]&0x80)) {valid=false; break;}
          payload.replace(at,4,"\0\0\0\1",4); at+=4+size;
        }
        if(!valid || at!=payload.size()) {
          if(++bad_frames_<4) Report("DIAG FAIL mirror decryption invalid NAL "+HexBytes(payload,16));
          continue;
        }
        if(!mirror_config_.empty()) {payload=mirror_config_+payload; mirror_config_.clear();}
        if(!url_video_.active() && !video_.started() && !mirror_codec_.empty()){video_.Start(1920,1080);video_.Config(mirror_codec_,0,0);video_epoch_=-1;}
        if (video_epoch_<0) video_epoch_=timestamp;
        if(capture_frame_) {
          pp::VarArrayBuffer frame(payload.size());memcpy(frame.Map(),payload.data(),payload.size());frame.Unmap();PostMessage(frame);capture_frame_=false;
        }
        if(!url_video_.active()) video_.Feed(payload, timestamp-video_epoch_);
      }
    }
  }

  void StartAudioUdp() {
    for(int i=0;i<2;++i) {
      audio_udp_[i]=pp::UDPSocket(this); uint8_t any[4]={0,0,0,0};
      audio_udp_[i].Bind(MakeIPv4(this,any,7101+i),callback_factory_.NewCallback(&AirPlayInstance::AudioBound,i));
    }
  }
  void AudioBound(int32_t r,int i) {
    Report("DIAG audio UDP bound port="+IndexText(7101+i)+" result="+IndexText(r));
    if(r==PP_OK) ReadAudio(i);
  }
  void ReadAudio(int i) {
    audio_buf_[i].resize(4096);
    audio_udp_[i].RecvFrom(&audio_buf_[i][0],4096,callback_factory_.NewCallbackWithOutput(&AirPlayInstance::AudioReceived,i));
  }
  void AudioReceived(int32_t n,const pp::NetAddress& peer,int i) {
    if(!session_peer_.empty() && peer.DescribeAsString(false).AsString()!=session_peer_){ReadAudio(i);return;}
    if(n>=20 && i==1 && audio_active_ && (static_cast<unsigned char>(audio_buf_[i][1])&127)==0x54) {
      const unsigned char* p=reinterpret_cast<const unsigned char*>(&audio_buf_[i][0]);
      audio_sync_rtp_=(uint32_t(p[4])<<24)|(uint32_t(p[5])<<16)|(uint32_t(p[6])<<8)|p[7];
      uint64_t stamp=0;for(int j=8;j<16;++j)stamp=(stamp<<8)|p[j];
      audio_sync_ntp_=double(stamp>>32)+double(stamp&0xffffffffULL)/4294967296.0;
      if(!audio_sync_) {audio_.ClearQueued();Report("DIAG audio sender clock synchronized");}
      audio_sync_=true;
    }
    if(n>12 && audio_active_) {
      const unsigned char* b=reinterpret_cast<const unsigned char*>(&audio_buf_[i][0]);
      int off=(i==1 && (b[1]&127)==86)?4:0;
      if(n>off+12 && ((b[off+1]&127)==96 || (b[off+1]&127)==97)) {
        b+=off; int head=12+4*(b[0]&15);
        if((b[0]&16) && n>off+head+4) head+=4+4*((b[head+2]<<8)|b[head+3]);
        if(n>off+head) {
          uint16_t seq=(static_cast<uint16_t>(b[2])<<8)|b[3];
          uint32_t stamp=(static_cast<uint32_t>(b[4])<<24)|(static_cast<uint32_t>(b[5])<<16)|(static_cast<uint32_t>(b[6])<<8)|b[7];
          if(!audio_epoch_set_) {audio_epoch_=stamp;audio_epoch_set_=true;}
          AudioPacket packet;packet.sequence=seq;packet.timestamp=stamp;
          packet.bytes.assign(reinterpret_cast<const char*>(b+head),n-off-head);
          audio_queue_.Push(packet);
          while(audio_queue_.Pop(pp::Module::Get()->core()->GetTimeTicks(),&packet)) {
          std::string data=packet.bytes;stamp=packet.timestamp;
          AES_ctx ctx; AES_init_ctx_iv(&ctx,aes_key_,aes_iv_);
          size_t blocks=data.size()/16*16;
          if(blocks) AES_CBC_decrypt_buffer(&ctx,reinterpret_cast<unsigned char*>(&data[0]),blocks);
          if(capture_audio_ && data.size()>16) {
            pp::VarArrayBuffer frame(data.size());memcpy(frame.Map(),data.data(),data.size());frame.Unmap();PostMessage(frame);capture_audio_=false;
            Report("DIAG captured decrypted audio bytes="+IndexText(data.size()));
          }
          bool sync=audio_sync_ && video_epoch_>=0 && video_.has_clock();
          double pts=sync?audio_sync_ntp_+static_cast<double>(static_cast<int32_t>(stamp-audio_sync_rtp_))/44100-video_epoch_:static_cast<double>(static_cast<int32_t>(stamp-audio_epoch_))/44100;
          if(sync && (pts< -10 || pts-video_.PlaybackTime()>30))sync=false;
          audio_.SetClock(video_.PlaybackTime(),sync);
          if(audio_have_stamp_) {
            int missing=static_cast<int32_t>(stamp-audio_last_stamp_)/480-1;
            for(int lost=std::min(missing,4);lost>0;--lost)audio_.Feed(std::string(),pts-lost*480.0/44100);
          }
          audio_last_stamp_=stamp;audio_have_stamp_=true;
          audio_.Feed(data,pts);
          if(++audio_packets_==1) Report("DIAG audio RTP received bytes="+IndexText(data.size()));
          if(audio_packets_%500==0)Report("DIAG RTP reordered duplicates="+IndexText(audio_queue_.duplicates())+" missing="+IndexText(audio_queue_.lost())+" synchronized="+(sync?"true":"false"));
          }
        }
      }
    }
    if(n!=PP_ERROR_ABORTED) ReadAudio(i);
  }

  void StartNtp() {
    ntp_socket_ = pp::UDPSocket(this);
    uint8_t any[4] = {0, 0, 0, 0};
    pp::NetAddress addr = MakeIPv4(this, any, kNtpPort);
    int32_t r = ntp_socket_.Bind(
        addr, callback_factory_.NewCallback(&AirPlayInstance::OnNtpBind));
    std::ostringstream oss;
    oss << "DIAG ntp Bind 0.0.0.0:" << kNtpPort << " return=" << r << " ("
        << ResultName(r) << ")";
    Report(oss.str());
  }

  void OnNtpBind(int32_t result) {
    if (result != PP_OK) {
      std::ostringstream oss;
      oss << "DIAG FAIL ntp Bind callback result=" << result << " ("
          << ResultName(result) << ")";
      Report(oss.str());
      return;
    }
    ntp_bound_ = true;
    Report("DIAG ntp listening on UDP " + PortText(kNtpPort));
    ArmNtpRead();
  }

  void ArmNtpRead() {
    ntp_buf_.resize(512);
    int32_t r = ntp_socket_.RecvFrom(
        &ntp_buf_[0], static_cast<int32_t>(ntp_buf_.size()),
        callback_factory_.NewCallbackWithOutput(&AirPlayInstance::OnNtpRecv));
    if (r != PP_OK_COMPLETIONPENDING) {
      std::ostringstream oss;
      oss << "DIAG FAIL ntp RecvFrom return=" << r << " (" << ResultName(r)
          << ")";
      Report(oss.str());
    }
  }

  void SendTiming(int32_t) {
    if (!timing_active_ || timing_peer_.is_null()) return;
    timing_request_.assign(32, '\0');
    timing_request_[0] = static_cast<char>(0x80);
    timing_request_[1] = static_cast<char>(0xd2);
    timing_request_[3] = 7;
    double now = pp::Module::Get()->core()->GetTime() + 2208988800.0;
    uint32_t seconds = static_cast<uint32_t>(now);
    uint32_t fraction = static_cast<uint32_t>((now - seconds) * 4294967296.0);
    for (int i=0;i<4;++i) {
      timing_request_[24+i]=static_cast<char>(seconds >> (24-8*i));
      timing_request_[28+i]=static_cast<char>(fraction >> (24-8*i));
    }
    ntp_socket_.SendTo(timing_request_.data(),32,timing_peer_,
      callback_factory_.NewCallback(&AirPlayInstance::TimingSent));
  }
  void TimingSent(int32_t r) {
    if (r < 0) Report("DIAG FAIL timing send="+IndexText(r));
    pp::Module::Get()->core()->CallOnMainThread(1000,
       callback_factory_.NewCallback(&AirPlayInstance::SendTiming));
  }
  void OnNtpRecv(int32_t result, const pp::NetAddress& addr) {
    if (result >= 32) {
      ++ntp_packets_;
      if (ntp_packets_<=3) Report("DIAG timing reply from " + addr.DescribeAsString(true).AsString());
    }
    ArmNtpRead();
  }

  void OnNtpSent(int32_t result) {
    std::ostringstream oss;
    oss << "DIAG ntp reply written (send=" << result << ")";
    Report(oss.str());
  }

  // ---- M2b: UDP bind + multicast ------------------------------------------

  void StartUdp() {
    udp_socket_ = pp::UDPSocket(this);
    uint8_t any[4] = {0, 0, 0, 0};
    pp::NetAddress addr = MakeIPv4(this, any, kUdpPort);
    int32_t r = udp_socket_.Bind(
        addr, callback_factory_.NewCallback(&AirPlayInstance::OnUdpBind));
    std::ostringstream oss;
    oss << "DIAG udp Bind 0.0.0.0:" << kUdpPort << " return=" << r << " ("
        << ResultName(r) << ")";
    Report(oss.str());
  }

  void OnUdpBind(int32_t result) {
    {
      std::ostringstream oss;
      oss << "DIAG udp Bind callback result=" << result << " ("
          << ResultName(result) << ")";
      Report(oss.str());
    }
    if (result != PP_OK)
      return;
    udp_bound_ = true;

    pp::NetAddress group = MakeIPv4(this, kMulticastAddr, kUdpPort);
    int32_t r = udp_socket_.JoinGroup(
        group, callback_factory_.NewCallback(&AirPlayInstance::OnUdpJoin));
    std::ostringstream oss;
    oss << "DIAG udp JoinGroup 239.255.255.250:" << kUdpPort << " return=" << r
        << " (" << ResultName(r) << ")";
    Report(oss.str());
  }

  void OnUdpJoin(int32_t result) {
    {
      std::ostringstream oss;
      oss << "DIAG udp JoinGroup callback result=" << result << " ("
          << ResultName(result) << ")";
      Report(oss.str());
    }
    if (result != PP_OK)
      return;
    udp_joined_ = true;

    SendUdpPing();
  }

  // Send to the group, then wait for our own datagram to come back to us.
  void SendUdpPing() {
    pp::NetAddress group = MakeIPv4(this, kMulticastAddr, kUdpPort);
    const char payload[] = "AIRPLAY-NACL-M2-UDP";
    int32_t r = udp_socket_.SendTo(
        payload, sizeof(payload) - 1, group,
        callback_factory_.NewCallback(&AirPlayInstance::OnUdpSendTo));
    std::ostringstream oss;
    oss << "DIAG udp SendTo 239.255.255.250 return=" << r << " ("
        << ResultName(r) << ")";
    Report(oss.str());
  }

  void OnUdpSendTo(int32_t result) {
    {
      std::ostringstream oss;
      oss << "DIAG udp SendTo callback result=" << result << " ("
          << ResultName(result) << ")";
      Report(oss.str());
    }
    ArmUdpRecv();
  }

  void OnUdpRecvFrom(int32_t result, const pp::NetAddress& addr) {
    udp_recv_pending_ = false;
    if (result > 0)
      udp_recv_ok_ = true;
    std::ostringstream oss;
    oss << "DIAG udp RecvFrom from " << addr.DescribeAsString(true).AsString()
        << " bytes=" << result;
    if (result > 0)
      oss << " payload=\"" << std::string(&udp_recv_buffer_[0], result)
          << "\"";
    oss << " — M2b UDP multicast reached";
    Report(oss.str());
  }

  // ---- M3: mDNS ------------------------------------------------------------
  //
  // macOS finds AirPlay receivers only through Bonjour, so until this answers
  // queries for _airplay._tcp / _raop._tcp the TV is invisible no matter how
  // healthy its sockets are.

  // The A record we publish has to name this TV's LAN address.  There is no
  // Pepper call that returns it, so learn it by connecting to the workstation
  // (whose address the page already knows) and reading the local endpoint the
  // kernel picked for that route.
  //
  // Discovery does not wait on this: the probe can take tens of seconds to
  // fail if the workstation is asleep, so mDNS starts immediately with the
  // assumed address and re-announces only if the probe disagrees.
  void StartMdns() {
    mdns_socket_ = pp::UDPSocket(this);

    // The TV runs its own discovery daemon and will already hold :5353.
    // ADDRESS_REUSE is a hint — if it is refused we bind anyway and let the
    // bind result say whether we lost the race.
    int32_t r = mdns_socket_.SetOption(
        PP_UDPSOCKET_OPTION_ADDRESS_REUSE, pp::Var(true),
        callback_factory_.NewCallback(
            &AirPlayInstance::OnMdnsAddressReuse));
    std::ostringstream oss;
    oss << "DIAG mdns ADDRESS_REUSE return=" << r << " (" << ResultName(r)
        << ")";
    Report(oss.str());
    if (r != PP_OK_COMPLETIONPENDING)
      BindMdns();
  }

  void OnMdnsAddressReuse(int32_t result) {
    std::ostringstream oss;
    oss << "DIAG mdns ADDRESS_REUSE result=" << result << " ("
        << ResultName(result) << ")";
    Report(oss.str());
    BindMdns();
  }

  void BindMdns() {
    if (mdns_bind_started_)
      return;
    mdns_bind_started_ = true;
    uint8_t any[4] = {0, 0, 0, 0};
    int32_t r = mdns_socket_.Bind(
        MakeIPv4(this, any, kMdnsPort),
        callback_factory_.NewCallback(&AirPlayInstance::OnMdnsBind));
    std::ostringstream oss;
    oss << "DIAG mdns Bind 0.0.0.0:" << kMdnsPort << " return=" << r << " ("
        << ResultName(r) << ")";
    Report(oss.str());
  }

  void OnMdnsBind(int32_t result) {
    {
      std::ostringstream oss;
      oss << "DIAG mdns Bind callback result=" << result << " ("
          << ResultName(result) << ")";
      Report(oss.str());
    }
    if (result != PP_OK)
      return;
    mdns_bound_ = true;

    pp::NetAddress group = MakeIPv4(this, kMdnsAddr, kMdnsPort);
    int32_t r = mdns_socket_.JoinGroup(
        group, callback_factory_.NewCallback(&AirPlayInstance::OnMdnsJoin));
    std::ostringstream oss;
    oss << "DIAG mdns JoinGroup 224.0.0.251:" << kMdnsPort << " return=" << r
        << " (" << ResultName(r) << ")";
    Report(oss.str());
  }

  void OnMdnsJoin(int32_t result) {
    if (result != PP_OK) {
      std::ostringstream oss;
      oss << "DIAG FAIL mdns JoinGroup callback result=" << result << " ("
          << ResultName(result) << ")";
      Report(oss.str());
      return;
    }
    mdns_joined_ = true;
    Report("DIAG mdns listening on 224.0.0.251:5353 — M3 armed");
    ArmMdnsRecv();
    Announce("startup");
  }

  void Announce(const std::string& why) {
    if(!mdns_joined_ || !local_ip_[0]) return;
    std::vector<uint8_t> packet = BuildRecords();
    SendMdns(packet, true, pp::NetAddress());
    std::ostringstream oss;
    oss << "DIAG mdns announced (" << why << ") " << packet.size()
        << " bytes, 7 records";
    Report(oss.str());
  }

  void ArmMdnsRecv() {
    if (mdns_recv_pending_)
      return;
    mdns_recv_pending_ = true;
    mdns_buffer_.resize(2048);
    int32_t r = mdns_socket_.RecvFrom(
        &mdns_buffer_[0], static_cast<int32_t>(mdns_buffer_.size()),
        callback_factory_.NewCallbackWithOutput(
            &AirPlayInstance::OnMdnsRecvFrom));
    if (r != PP_OK_COMPLETIONPENDING) {
      mdns_recv_pending_ = false;
      std::ostringstream oss;
      oss << "DIAG FAIL mdns RecvFrom return=" << r << " (" << ResultName(r)
          << ")";
      Report(oss.str());
    }
  }

  void OnMdnsRecvFrom(int32_t result, const pp::NetAddress& addr) {
    mdns_recv_pending_ = false;
    if (result > 0) {
      ++mdns_queries_;
      std::string wanted;
      if (local_ip_[0] && PacketWantsUs(
              reinterpret_cast<const uint8_t*>(&mdns_buffer_[0]),
              static_cast<size_t>(result), &wanted)) {
        ++mdns_answered_;
        std::ostringstream oss;
        oss << "DIAG mdns query #" << mdns_queries_ << " from "
            << addr.DescribeAsString(true).AsString() << " for " << wanted
            << " — answering";
        Report(oss.str());
        std::vector<uint8_t> packet = BuildRecords();
        SendMdns(packet, false, addr);              // unicast, since it asked
        SendMdns(packet, true, pp::NetAddress());   // and to the group
      }
    }
    ArmMdnsRecv();
  }

  void SendMdns(const std::vector<uint8_t>& packet, bool multicast,
                const pp::NetAddress& dest) {
    const pp::NetAddress to =
        multicast ? MakeIPv4(this, kMdnsAddr, kMdnsPort) : dest;
    int32_t r = mdns_socket_.SendTo(
        reinterpret_cast<const char*>(&packet[0]),
        static_cast<int32_t>(packet.size()), to,
        callback_factory_.NewCallback(&AirPlayInstance::OnMdnsSend));
    if (r != PP_OK_COMPLETIONPENDING) {
      std::ostringstream oss;
      oss << "DIAG FAIL mdns SendTo return=" << r << " (" << ResultName(r)
          << ")";
      Report(oss.str());
    }
  }

  void OnMdnsSend(int32_t result) {
    if (result < 0) {
      std::ostringstream oss;
      oss << "DIAG FAIL mdns SendTo result=" << result << " ("
          << ResultName(result) << ")";
      Report(oss.str());
    }
  }

  // Is this packet a query for something we publish?  Anything else on the
  // group is somebody else's traffic and is dropped.
  bool PacketWantsUs(const uint8_t* p, size_t n, std::string* matched) {
    if (n < 12)
      return false;
    const uint16_t flags = static_cast<uint16_t>((p[2] << 8) | p[3]);
    if (flags & 0x8000)
      return false;                       // a response, not a query
    const uint16_t questions = static_cast<uint16_t>((p[4] << 8) | p[5]);
    size_t off = 12;
    for (uint16_t i = 0; i < questions; ++i) {
      std::string name;
      int hops = 32;
      if (!ReadName(p, n, &off, &name, &hops))
        return false;
      if (off + 4 > n)
        return false;
      off += 4;                           // qtype + qclass
      if (name.find("_airplay") != std::string::npos ||
          name.find("_raop") != std::string::npos ||
          name.find(kServiceName) != std::string::npos) {
        *matched = name;
        return true;
      }
    }
    return false;
  }

  std::vector<uint8_t> BuildRecords() {
    const std::string airplay =
        std::string(kServiceName) + "._airplay._tcp.local";
    const std::string raop =
        std::string(kRaopMac) + "@" + kServiceName + "._raop._tcp.local";

    std::vector<std::string> airplay_txt;
    airplay_txt.push_back(std::string("deviceid=") + kDeviceId);
    // Low word 0x527FFFF7, not 0x5A7FFFF7: bit 27 is "supports legacy pairing",
    // and it is the only reason macOS opens a session with POST /pair-setup.
    // With it clear the client skips pair-setup and pair-verify both and goes
    // straight to the legacy FairPlay /fp-setup exchange.  The high word 0x1E
    // (bits 33-35) is unchanged and keeps the HomeKit pairing bits off, which
    // is what makes the skip legal.
    airplay_txt.push_back("features=0x527FFFF7,0x1E");
    airplay_txt.push_back(std::string("srcvers=") + kSrcVersion);
    airplay_txt.push_back(std::string("model=") + kModel);
    airplay_txt.push_back("flags=0x4");
    airplay_txt.push_back(std::string("pi=") + kPairingId);
    airplay_txt.push_back("vv=2");

    std::vector<std::string> raop_txt;
    raop_txt.push_back("txtvers=1");
    raop_txt.push_back("ch=2");
    raop_txt.push_back("cn=0,1");
    raop_txt.push_back("et=0,3,5");
    raop_txt.push_back("md=0,1,2");
    raop_txt.push_back("pw=false");
    raop_txt.push_back("sr=44100");
    raop_txt.push_back("ss=16");
    raop_txt.push_back("tp=UDP");
    raop_txt.push_back("vn=65537");
    raop_txt.push_back(std::string("vs=") + kSrcVersion);
    raop_txt.push_back(std::string("am=") + kModel);
    raop_txt.push_back("sf=0x4");
    raop_txt.push_back("ek=1");
    raop_txt.push_back("sv=false");
    raop_txt.push_back("da=true");
    raop_txt.push_back("vv=2");

    std::vector<uint8_t> b;
    PutU16(&b, 0);                 // id — always 0 for mDNS
    PutU16(&b, 0x8400);            // response, authoritative
    PutU16(&b, 0);                 // questions
    PutU16(&b, 7);                 // answers
    PutU16(&b, 0);                 // authority
    PutU16(&b, 0);                 // additional

    AddPtr(&b, "_airplay._tcp.local", airplay);
    AddSrv(&b, airplay, kTcpPort, kHostName);
    AddTxt(&b, airplay, airplay_txt);
    AddPtr(&b, "_raop._tcp.local", raop);
    AddSrv(&b, raop, kTcpPort, kHostName);
    AddTxt(&b, raop, raop_txt);
    AddA(&b, kHostName, local_ip_);
    return b;
  }

  pp::CompletionCallbackFactory<AirPlayInstance> callback_factory_;
  bool capture_frame_=false;
  bool capture_audio_=false;
  VideoSink video_;
  AudioSink audio_;
  UrlVideo url_video_;
  std::string session_peer_;
  pp::UDPSocket audio_udp_[2];
  std::vector<char> audio_buf_[2];
  bool audio_active_=false,audio_epoch_set_=false;
  uint32_t audio_epoch_=0;
  int audio_packets_=0;
  bool audio_seq_set_=false;
  uint16_t audio_last_seq_=0;
  int audio_duplicates_=0;
  RtpQueue audio_queue_;
  bool audio_sync_=false,audio_have_stamp_=false;
  uint32_t audio_sync_rtp_=0,audio_last_stamp_=0;
  double audio_sync_ntp_=0;
  double video_epoch_=-1;
  AES_ctx mirror_aes_;
  unsigned char mirror_block_[16];
  int mirror_pos_=16, bad_frames_=0,mirror_packets_=0;
  bool mirror_ready_=false, timing_active_=false;
  std::string mirror_pending_, mirror_config_, timing_request_,mirror_codec_;
  pp::NetAddress timing_peer_;
  pp::Graphics2D graphics_2d_;
  pp::TCPSocket tcp_socket_;
  pp::UDPSocket udp_socket_;
  std::vector<char> udp_recv_buffer_;
  pp::TCPSocket probe_socket_;
  pp::UDPSocket mdns_socket_;
  std::vector<char> mdns_buffer_;
  // Assumed until the probe below says otherwise.
  uint8_t local_ip_[4] = {0, 0, 0, 0};

  // M4: the AirPlay HTTP session.  A pool, not a slot: the client holds the
  // control connection and an event connection to :7000 simultaneously, so the
  // read/write chain is serialised per connection rather than globally.
  static const int kMaxHttpConns = 4;
  struct HttpConn {
    pp::TCPSocket socket;
    std::string request;
    std::vector<char> read_buffer;
    std::string write_out;
    bool read_pending = false;
    bool write_pending = false;
    bool in_use = false;
  };
  HttpConn http_conns_[kMaxHttpConns];
  // The slot whose request is being handled.  Set for the duration of one
  // synchronous HandleRequest call; the reply path reads it.
  int cur_ = -1;
  int http_requests_ = 0;
  // AirPlay frames its session as RTSP, not HTTP: the reply has to carry the
  // same protocol token and the same CSeq as the request, or the client cannot
  // match it to what it asked.  Both are taken from the request in flight.
  bool rtsp_ = false;
  std::string cseq_;
  // FairPlay state: stage 2's 164-byte request has to survive until SETUP hands
  // us the ekey that it decrypts.
  fairplay_t fairplay_;

  // What actually succeeded, so the page can ask for it after Init() is over.
  bool tcp_bound_ = false;
  bool tcp_listening_ = false;
  bool tcp_accepted_ = false;
  bool udp_bound_ = false;
  bool udp_joined_ = false;
  bool udp_recv_ok_ = false;
  bool udp_recv_pending_ = false;
  bool mdns_bound_ = false;
  bool mdns_joined_ = false;
  bool mdns_recv_pending_ = false;
  bool mdns_bind_started_ = false;
  int mdns_queries_ = 0;
  int mdns_answered_ = 0;

  // M5: the stream listeners SETUP advertises.  Bound at startup and held open,
  // because the client dials into the mirror data port rather than the reverse.
  pp::TCPSocket stream_listen_[kStreamCount];
  pp::TCPSocket stream_conn_[kStreamCount];
  std::vector<char> stream_buf_[kStreamCount];
  bool stream_listening_[kStreamCount] = {false, false};
  int stream_conns_ = 0;

  // Timing, so the client's NTP exchange gets an answer.
  pp::UDPSocket ntp_socket_;
  std::vector<char> ntp_buf_;
  bool ntp_bound_ = false;
  int ntp_packets_ = 0;

  // SETUP: the key FairPlay handed over, and whether it arrived at all.
  unsigned char aes_key_[16];
  unsigned char aes_iv_[16];
  bool setup_done_ = false;
  int setup_requests_ = 0;
};

class AirPlayModule : public pp::Module {
 public:
  AirPlayModule() : pp::Module() {}
  virtual pp::Instance* CreateInstance(PP_Instance instance) {
    return new AirPlayInstance(instance);
  }
};

namespace pp {
Module* CreateModule() { return new AirPlayModule(); }
}  // namespace pp
