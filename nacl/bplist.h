// Apple binary property lists ("bplist00"), as far as AirPlay needs them.
//
// SETUP arrives as one of these, and its reply has to be one too: the client
// sends Content-Type: application/x-apple-binary-plist and parses the bytes it
// gets back, so answering with XML is not an option on either side.
//
// The reader keeps a value tree because SETUP's interesting fields sit three
// levels down -- root -> "streams" -> element -> "type" -- and the writer only
// ever produces the fixed shape of a SETUP reply, so it is a builder rather
// than a general encoder.
#ifndef AIRPLAY_BPLIST_H_
#define AIRPLAY_BPLIST_H_

#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

class BplistReader;

class Bplist {
 public:
  enum Kind { kNone, kUint, kData, kString, kDict, kArray };

  ~Bplist();

  // Returns a new tree, or NULL if `data` is not a bplist00 this reader
  // understands.  The caller owns the result.
  static Bplist* Parse(const char* data, size_t len);

  Kind kind() const { return kind_; }
  bool Is(Kind k) const { return kind_ == k; }
  uint64_t uint() const { return u_; }
  // String text, or the raw bytes when this node came from a data object.
  const std::string& str() const { return s_; }

  // Array elements, or dictionary entries.  For a dictionary, key(i) pairs
  // with value(i).
  size_t count() const { return children_.size(); }
  const Bplist* item(size_t i) const { return children_[i]; }
  const std::string& key(size_t i) const { return keys_[i]; }
  const Bplist* value(size_t i) const { return children_[i]; }

  // Dictionary lookup; NULL when absent.
  const Bplist* get(const char* key) const;

 private:
  Bplist();
  // The tree owns its children through raw pointers, so copies are refused.
  Bplist(const Bplist&);
  Bplist& operator=(const Bplist&);

  Kind kind_;
  uint64_t u_;
  std::string s_;
  std::vector<std::string> keys_;      // dictionary keys; empty for arrays
  std::vector<Bplist*> children_;

  friend class BplistReader;
};

// Builds the SETUP reply.  Each call returns an object index; hand the root's
// index to Encode().
class BplistBuilder {
 public:
  BplistBuilder() {}

  int Uint(uint64_t v);
  int Data(const std::string& d);
  int Str(const std::string& s);
  int Array(const std::vector<int>& items);
  // Keys become their own string objects, so the caller passes plain strings.
  int Dict(const std::vector<std::pair<std::string, int> >& pairs);

  std::string Encode(int root) const;

 private:
  enum { kUint, kData, kString, kArray, kDict };

  struct Obj {
    int tag;
    uint64_t u;
    std::string s;
    std::vector<int> refs;    // array items
    std::vector<int> keys;    // dictionary key object indices
    std::vector<int> values;  // dictionary value object indices
  };

  std::vector<Obj> objects_;
};

#endif  // AIRPLAY_BPLIST_H_
