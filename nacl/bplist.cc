#include "bplist.h"

#include <string.h>

namespace {

const size_t kHeaderLen = 8;
const size_t kTrailerLen = 32;
// Real SETUP bodies nest three levels; the cap is only here so a malformed
// offset table cannot turn into unbounded recursion.
const int kMaxDepth = 24;

uint64_t BeUint(const uint8_t* p, size_t n) {
  uint64_t v = 0;
  for (size_t i = 0; i < n; ++i) v = (v << 8) | static_cast<uint64_t>(p[i]);
  return v;
}

void AppendBe(std::string* out, uint64_t v, size_t n) {
  for (size_t i = 0; i < n; ++i)
    out->push_back(static_cast<char>((v >> (8 * (n - 1 - i))) & 0xFF));
}

// Integer width is 2^n bytes, as specified by Apple binary plists.
void AppendIntObject(std::string* out, uint64_t v) {
  if (v > 0x7fffffffffffffffULL) {
    out->push_back(static_cast<char>(0x14));
    out->append(8, '\0');
    AppendBe(out, v, 8);
    return;
  }
  size_t bytes = 1;
  size_t exponent = 0;
  while (bytes < 8 && (v >> (8 * bytes)) != 0) { bytes *= 2; ++exponent; }
  out->push_back(static_cast<char>(0x10 | exponent));
  AppendBe(out, v, bytes);
}

// The length field of a container or byte string: inline in the marker's low
// nibble, or -- when that nibble is 0xF -- in an integer object that follows.
void AppendCount(std::string* out, uint8_t nibble, size_t count) {
  if (count < 15) {
    out->push_back(static_cast<char>(nibble | count));
    return;
  }
  out->push_back(static_cast<char>(nibble | 0x0F));
  AppendIntObject(out, count);
}

bool ReadCount(const uint8_t* p, size_t n, size_t* off, uint8_t nibble,
               uint64_t* out) {
  if (nibble != 0x0F) {
    *out = nibble;
    return true;
  }
  if (*off >= n) return false;
  uint8_t marker = p[(*off)++];
  if ((marker & 0xF0) != 0x10) return false;
  size_t bytes = size_t(1) << (marker & 0x0F);
  if (bytes > 8 || *off + bytes > n) return false;
  *out = BeUint(p + *off, bytes);
  *off += bytes;
  return true;
}

}  // namespace

// Walks the offset table.  Objects are addressed by index rather than by
// pointer, which is what keeps a self-referential or repeated reference from
// turning into a cycle: each index is decoded once, as its own copy of the
// value.  An AirPlay SETUP body never shares an object, so the copies are
// indistinguishable from sharing and the tree stays trivially owned.
class BplistReader {
 public:
  BplistReader(const uint8_t* data, size_t len) : p_(data), n_(len) {}

  Bplist* Parse() {
    if (n_ < kHeaderLen + kTrailerLen) return NULL;
    if (memcmp(p_, "bplist00", kHeaderLen) != 0) return NULL;

    const uint8_t* t = p_ + n_ - kTrailerLen;
    offset_size_ = t[6];
    ref_size_ = t[7];
    num_objects_ = BeUint(t + 8, 8);
    top_ = BeUint(t + 16, 8);
    table_off_ = static_cast<size_t>(BeUint(t + 24, 8));

    if (offset_size_ < 1 || offset_size_ > 8) return NULL;
    if (ref_size_ < 1 || ref_size_ > 8) return NULL;
    if (num_objects_ == 0 || num_objects_ > (1u << 20)) return NULL;
    if (table_off_ > n_) return NULL;
    if (num_objects_ * offset_size_ > n_ - table_off_) return NULL;

    return Decode(top_, 0);
  }

 private:
  bool OffsetOf(uint64_t index, size_t* out) const {
    if (index >= num_objects_) return false;
    size_t at = table_off_ + static_cast<size_t>(index) * offset_size_;
    if (at + offset_size_ > n_) return false;
    *out = static_cast<size_t>(BeUint(p_ + at, offset_size_));
    return true;
  }

  Bplist* Decode(uint64_t index, int depth) {
    if (depth > kMaxDepth) return NULL;
    size_t at;
    if (!OffsetOf(index, &at) || at >= n_) return NULL;

    uint8_t marker = p_[at];
    size_t off = at + 1;
    uint8_t hi = static_cast<uint8_t>(marker >> 4);
    uint8_t lo = static_cast<uint8_t>(marker & 0x0F);

    Bplist* node = new Bplist();

    if (hi == 0x0) {  // null, false, true
      node->kind_ = Bplist::kUint;
      node->u_ = (lo == 0x9) ? 1 : 0;
      return node;
    }

    if (hi == 0x1) {  // integer
      size_t bytes = size_t(1) << lo;
      if (off + bytes > n_) {
        delete node;
        return NULL;
      }
      node->kind_ = Bplist::kUint;
      node->u_ = BeUint(p_ + off, bytes);
      return node;
    }

    if (hi == 0x2 && (lo == 2 || lo == 3)) {
      size_t bytes=size_t(1)<<lo;
      if(off+bytes>n_) {delete node;return NULL;}
      node->kind_=Bplist::kReal;
      if(bytes==4) {uint32_t bits=BeUint(p_+off,4);float v;memcpy(&v,&bits,4);node->real_=v;}
      else {uint64_t bits=BeUint(p_+off,8);memcpy(&node->real_,&bits,8);}
      return node;
    }
    if (hi == 0x4 || hi == 0x5 || hi == 0x6) {  // data / ASCII / UTF-16BE
      uint64_t len;
      if (!ReadCount(p_, n_, &off, lo, &len)) {
        delete node;
        return NULL;
      }
      size_t width = (hi == 0x6) ? 2 : 1;
      if (len > (n_ - off) / width) {
        delete node;
        return NULL;
      }
      node->kind_ = (hi == 0x4) ? Bplist::kData : Bplist::kString;
      if (hi == 0x6) {
        // Every key and value AirPlay puts in a SETUP body is Latin, so a code
        // unit above 0x7F can only come from something we do not need; mark it
        // rather than mangle it, so a wrong decode is visible in the log.
        for (uint64_t i = 0; i < len; ++i) {
          uint16_t u = static_cast<uint16_t>((p_[off + 2 * i] << 8) |
                                             p_[off + 2 * i + 1]);
          node->s_.push_back(u < 0x80 ? static_cast<char>(u) : '?');
        }
      } else {
        node->s_.assign(reinterpret_cast<const char*>(p_ + off),
                        static_cast<size_t>(len));
      }
      return node;
    }

    if (hi == 0xA || hi == 0xD) {  // array / dictionary
      uint64_t count;
      if (!ReadCount(p_, n_, &off, lo, &count)) {
        delete node;
        return NULL;
      }
      // Every entry costs at least one object reference.
      if (count > (n_ - off) / ref_size_) {
        delete node;
        return NULL;
      }
      // A dictionary stores all its keys first, then all its values.
      size_t pairs = static_cast<size_t>(count);
      size_t refs = (hi == 0xA) ? pairs : pairs * 2;
      std::vector<uint64_t> ref(refs);
      for (size_t i = 0; i < refs; ++i) {
        ref[i] = BeUint(p_ + off, ref_size_);
        off += ref_size_;
      }

      node->kind_ = (hi == 0xA) ? Bplist::kArray : Bplist::kDict;
      for (size_t i = 0; i < pairs; ++i) {
        if (hi == 0xA) {
          Bplist* child = Decode(ref[i], depth + 1);
          if (!child) {
            delete node;
            return NULL;
          }
          node->children_.push_back(child);
          continue;
        }
        Bplist* key = Decode(ref[i], depth + 1);
        Bplist* value = Decode(ref[pairs + i], depth + 1);
        if (!key || !value || !key->Is(Bplist::kString)) {
          delete key;
          delete value;
          delete node;
          return NULL;
        }
        node->keys_.push_back(key->str());
        node->children_.push_back(value);
      }
      return node;
    }

    // Reals (0x2), dates (0x3), UIDs (0x8) and sets (0xC).  Nothing in an
    // AirPlay SETUP body is any of these, but a body containing one must not
    // lose its whole enclosing container to it: NULL propagates through every
    // parent, so one unrecognised scalar would surface as "the client sent no
    // streams at all" -- a wrong diagnosis that nothing downstream can see.
    // A hole is honest, and NodeText renders it as "(null)".
    if (hi == 0x2 || hi == 0x3 || hi == 0x8 || hi == 0xC) {
      node->kind_ = Bplist::kNone;
      return node;
    }

    // Markers that are not plist at all (0x7, 0x9, 0xB, 0xE, 0xF) still fail
    // the document.  They mean the offset table is being walked wrongly, and a
    // hole would hide that rather than report it.
    delete node;
    return NULL;
  }

  const uint8_t* p_;
  size_t n_;
  size_t offset_size_;
  size_t ref_size_;
  uint64_t num_objects_;
  uint64_t top_;
  size_t table_off_;
};

Bplist::Bplist() : kind_(kNone), u_(0) {}

Bplist::~Bplist() {
  for (size_t i = 0; i < children_.size(); ++i) delete children_[i];
}

Bplist* Bplist::Parse(const char* data, size_t len) {
  if (!data) return NULL;
  BplistReader reader(reinterpret_cast<const uint8_t*>(data), len);
  return reader.Parse();
}

const Bplist* Bplist::get(const char* key) const {
  for (size_t i = 0; i < keys_.size(); ++i)
    if (keys_[i] == key) return children_[i];
  return NULL;
}

int BplistBuilder::Uint(uint64_t v) {
  Obj o;
  o.tag = kUint;
  o.u = v;
  objects_.push_back(o);
  return static_cast<int>(objects_.size()) - 1;
}

int BplistBuilder::Data(const std::string& d) {
  Obj o;
  o.tag = kData;
  o.s = d;
  objects_.push_back(o);
  return static_cast<int>(objects_.size()) - 1;
}

int BplistBuilder::Str(const std::string& s) {
  Obj o;
  o.tag = kString;
  o.s = s;
  objects_.push_back(o);
  return static_cast<int>(objects_.size()) - 1;
}

int BplistBuilder::Array(const std::vector<int>& items) {
  Obj o;
  o.tag = kArray;
  o.refs = items;
  objects_.push_back(o);
  return static_cast<int>(objects_.size()) - 1;
}

int BplistBuilder::Dict(const std::vector<std::pair<std::string, int> >& pairs) {
  Obj o;
  o.tag = kDict;
  for (size_t i = 0; i < pairs.size(); ++i) {
    o.keys.push_back(Str(pairs[i].first));
    o.values.push_back(pairs[i].second);
  }
  objects_.push_back(o);
  return static_cast<int>(objects_.size()) - 1;
}

std::string BplistBuilder::Encode(int root) const {
  // One byte addresses 256 objects, which is far more than a SETUP reply uses.
  const size_t ref_size = (objects_.size() < 256) ? 1 : 2;

  std::string out;
  out.append("bplist00", kHeaderLen);

  std::vector<size_t> offsets;
  offsets.reserve(objects_.size());
  for (size_t i = 0; i < objects_.size(); ++i) {
    offsets.push_back(out.size());
    const Obj& o = objects_[i];
    switch (o.tag) {
      case kUint: {
        AppendIntObject(&out, o.u);
        break;
      }
      case kData:
      case kString: {
        AppendCount(&out, (o.tag == kData) ? 0x40 : 0x50, o.s.size());
        out.append(o.s);
        break;
      }
      case kArray: {
        AppendCount(&out, 0xA0, o.refs.size());
        for (size_t j = 0; j < o.refs.size(); ++j)
          AppendBe(&out, static_cast<uint64_t>(o.refs[j]), ref_size);
        break;
      }
      case kDict: {
        AppendCount(&out, 0xD0, o.keys.size());
        for (size_t j = 0; j < o.keys.size(); ++j)
          AppendBe(&out, static_cast<uint64_t>(o.keys[j]), ref_size);
        for (size_t j = 0; j < o.values.size(); ++j)
          AppendBe(&out, static_cast<uint64_t>(o.values[j]), ref_size);
        break;
      }
    }
  }

  size_t table_off = out.size();
  size_t offset_size = 1;
  while (offset_size < 8 && (table_off >> (8 * offset_size)) != 0) ++offset_size;
  for (size_t i = 0; i < offsets.size(); ++i)
    AppendBe(&out, static_cast<uint64_t>(offsets[i]), offset_size);

  // The trailer: 5 reserved bytes, the sort version, the two element sizes,
  // then the object count, the root's index and the offset table's position.
  std::string trailer(5, '\0');
  trailer.push_back('\0');
  trailer.push_back(static_cast<char>(offset_size));
  trailer.push_back(static_cast<char>(ref_size));
  AppendBe(&trailer, objects_.size(), 8);
  AppendBe(&trailer, static_cast<uint64_t>(root), 8);
  AppendBe(&trailer, table_off, 8);
  out.append(trailer);

  return out;
}
