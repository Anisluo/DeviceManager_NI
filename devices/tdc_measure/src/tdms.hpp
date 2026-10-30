// tdms.hpp — minimal TDMS reader / writer for NI Digital Pattern Editor files.
//
// .digipat / .digicapture written by the editor are small little-endian, non-interleaved TDMS
// files. read() keeps every property and channel type plus the byte position of each raw data
// block, so a fixed-size value can be patched in place (patch_u32) and a whole file can be
// written back with new values (write, one segment per object).
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace tdms {

enum Type : uint32_t {
    I8 = 1, I16 = 2, I32 = 3, I64 = 4, U8 = 5, U16 = 6, U32 = 7, U64 = 8, SGL = 9, DBL = 10,
    STRING = 0x20, BOOL = 0x21, TIMESTAMP = 0x44
};

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Prop {
    std::string name;
    uint32_t type = STRING;
    double num = 0;          // numeric, bool and timestamp (seconds) values
    std::string str;         // string values
};

struct Chunk {
    size_t offset = 0;       // byte offset of the first value in the file
    size_t count = 0;
    uint32_t type = U8;
};

struct Channel {
    std::string name;
    uint32_t type = U8;
    std::vector<double> nums;          // every type except STRING
    std::vector<std::string> strs;     // STRING
    std::vector<Prop> props;
    std::vector<Chunk> chunks;
    size_t size() const { return type == STRING ? strs.size() : nums.size(); }
};

struct Group {
    std::string name;
    std::vector<Prop> props;
    std::vector<Channel> channels;
    Channel* channel(const std::string& n);
    const Channel* channel(const std::string& n) const;
};

struct File {
    std::vector<Prop> props;
    std::vector<Group> groups;
    Group* group(const std::string& n);
    const Group* group(const std::string& n) const;
    const Channel* channel(const std::string& g, const std::string& c) const;
    Channel* channel(const std::string& g, const std::string& c);
};

// Property helpers (props keep their order; set() keeps the type of an existing property).
const Prop* find_prop(const std::vector<Prop>& props, const std::string& name);
void set_prop(std::vector<Prop>& props, const std::string& name, double num);
void set_prop(std::vector<Prop>& props, const std::string& name, const std::string& str);

File read(const std::string& bytes);
std::string write(const File& f);
// Copy of `bytes` with element `index` of a U32 channel replaced.
std::string patch_u32(const std::string& bytes, const File& f, const std::string& group,
                      const std::string& channel, size_t index, uint32_t value);

} // namespace tdms
