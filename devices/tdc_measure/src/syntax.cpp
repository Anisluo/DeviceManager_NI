#include "syntax.hpp"
#include "util.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace tdc {

namespace {

constexpr unsigned OP_REPEAT = 1, OP_SET_LOOP = 4, OP_END_LOOP = 5, OP_JUMP_IF = 7, OP_HALT = 15;
constexpr unsigned FLAG_MATCH = 0x20, FLAG_ON_MATCHED = 0x28;
constexpr long MATCH_LATENCY = 80;   // cycles from a comparison to the jump that reads it
constexpr size_t MAX_LOOP_DEPTH = 8;

const std::set<std::string>& reserved_words()
{
    static const std::set<std::string> words = [] {
        std::set<std::string> w{"call", "capture", "capture_start", "capture_stop", "clear_seqflag", "clear_signal",
                                "edge_multiplier", "end_loop", "export", "exit_loop", "exit_loop_if", "failed",
                                "file_format_version", "halt", "import", "jump", "jump_if", "keep_alive",
                                "keep_alive_pattern", "match", "matched", "pattern", "pulse_signal", "repeat",
                                "reset_trigger", "return", "scan", "scan_in", "scan_out", "set_loop", "set_seqflag",
                                "set_signal", "source", "source_d_replace", "source_start", "timeset", "write_reg"};
        for (int i = 0; i < 4; ++i) {
            w.insert("event" + std::to_string(i));
            w.insert("seqflag" + std::to_string(i));
        }
        for (int i = 0; i < 16; ++i) w.insert("reg" + std::to_string(i));
        return w;
    }();
    return words;
}

} // namespace

std::string identifier_problem(const std::string& name)
{
    if (!valid_name(name)) return "名称 '" + name + "' 无效：必须以字母或下划线开头，只能用 A-Z、a-z、0-9 和下划线";
    if (reserved_words().count(name)) return "名称 '" + name + "' 是 NI 图形语言的保留字，不能使用";
    return {};
}

bool is_unencoded_opcode(const std::string& name)
{
    static const std::set<std::string> names{
        "jump", "exit_loop", "exit_loop_if", "call", "return", "keep_alive", "scan", "set_seqflag", "clear_seqflag",
        "write_reg", "set_signal", "pulse_signal", "clear_signal", "reset_trigger", "capture_start", "capture",
        "capture_stop", "source_start", "source", "source_d_replace"};
    return names.count(name) > 0;
}

std::vector<Issue> check_syntax(const Pattern& p)
{
    std::vector<Issue> out;
    auto add = [&out](int v, bool error, std::string text) { out.push_back(Issue{v, error, std::move(text)}); };
    const std::vector<uint32_t> in = p.ins();
    const size_t n = in.size();
    if (n == 0) {
        add(-1, true, "图形至少要有一个向量");
        return out;
    }
    std::vector<Decoded> dec;
    for (uint32_t v : in) dec.push_back(decode(v));
    auto cycles = [&dec](size_t i) { return dec[i].op == OP_REPEAT ? long(dec[i].operand) : 1L; };
    auto has_compare = [&p](size_t i) { return p.states[i].find_first_of("LHMVE") != std::string::npos; };

    // labels
    std::map<int, std::string> label_at;
    std::set<std::string> seen;
    for (size_t k = 0; k < p.label_names.size() && k < p.label_locs.size(); ++k) {
        const std::string& name = p.label_names[k];
        const int at = p.label_locs[k];
        const std::string problem = identifier_problem(name);
        if (!problem.empty()) add(at, true, problem);
        if (!seen.insert(name).second) add(at, true, "标签 " + name + " 重复");
        if (at < 0 || size_t(at) >= n) add(-1, true, "标签 " + name + " 不在任何向量上");
    }
    auto target = [&p](unsigned label) { return label < p.label_locs.size() ? p.label_locs[label] : -1; };

    // loops, in file order: a loop block is written between its set_loop and end_loop
    std::vector<size_t> open;
    for (size_t i = 0; i < n; ++i) {
        const Decoded& d = dec[i];
        if (d.op == OP_REPEAT || d.op == OP_SET_LOOP) {
            if (d.operand < 1) add(int(i), true, "次数应在 1 … 65535");
        }
        if (d.op == OP_SET_LOOP) {
            open.push_back(i);
            if (open.size() > MAX_LOOP_DEPTH)
                add(int(i), false, "循环嵌套超过 8 层，运行时循环栈会溢出（编译时不检查）");
        } else if (d.op == OP_END_LOOP) {
            const unsigned label = d.operand & 0x7FFF;
            const int to = target(label);
            if (to < 0) {
                add(int(i), true, "end_loop 引用的标签不存在");
            } else if (open.empty()) {
                add(int(i), true, "end_loop 没有对应的 set_loop");
            } else {
                if (size_t(to) <= open.back())
                    add(int(i), true, "end_loop 的标签要标在 set_loop 之后的循环第一个向量上");
                else if (size_t(to) > i)
                    add(int(i), true, "end_loop 的标签要在 end_loop 之前");
                open.pop_back();
            }
        } else if (d.op == OP_JUMP_IF) {
            if (target(d.operand & 0x7FFF) < 0) add(int(i), true, "jump_if 引用的标签不存在");
        }
    }
    for (size_t at : open) add(int(at), true, "set_loop 没有对应的 end_loop");

    // comparisons
    for (size_t i = 0; i < n; ++i) {
        const Decoded& d = dec[i];
        if (d.op != OP_JUMP_IF && (d.flags & FLAG_MATCH) && !has_compare(i))
            add(int(i), false, "match 向量上没有比较状态（H、L、M、V、E），不会产生匹配结果");
        if (d.op != OP_JUMP_IF || d.flags != FLAG_ON_MATCHED) continue;
        // a vector that matches and jumps to itself is a match pipeline
        if (target(d.operand & 0x7FFF) == int(i)) continue;
        // matched reads the vector exactly 80 cycles back; only straight-line code can be walked
        long back = 0;
        for (size_t j = i; j-- > 0;) {
            const unsigned op = dec[j].op;
            if (op != 0 && op != OP_REPEAT) break;
            back += cycles(j);
            if (back < MATCH_LATENCY) continue;
            if (!(dec[j].flags & FLAG_MATCH))
                add(int(i), false, "matched 读取的是正好 80 个周期前的向量，那个向量（v" + std::to_string(j) + "）没有 match");
            break;
        }
    }

    if (dec[n - 1].op != OP_HALT) add(int(n - 1), true, "最后一个向量必须是 halt，否则运行行为未定义");
    std::stable_sort(out.begin(), out.end(), [](const Issue& a, const Issue& b) { return a.vector < b.vector; });
    return out;
}

} // namespace tdc
