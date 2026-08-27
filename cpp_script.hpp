// One-header helper for writing scripts in C++
// Requires C++20

// Pavlygin Danil 2026-2026

// This file contains:
// [1] print, println
// [2] Filesystem IO functions, file search
// [3] File loader/parser for storing script data
// [4] SHA256 hash, file contents change detection
// [5] [argc, argv] handlers - parser, table, table entries
// [6] command - helper for constructing and running terminal commands
// [7] Auto-rebuild - run rebuild command if script has been modified
// [8] Library import facilities

// Changelog:
// 1.1  Added make_import_guide, import
// 1.0  Fixed changes fixation, format specializations
// 0.93 Added info_file
// 0.92 Added path, command, args_parser formatter specializations
// 0.91 Rewrited filesystem IO
// 0.9  Added print, println
// 0.8  Optimized try_rebuild_itself() - storing hashes in txt file
// 0.7  Added try_rebuild_itself()
// 0.6  Added sha256 hash and file_contents_changed()
// 0.5  Added binary file write/read functions
// 0.4  args_parser now satisfies cmd_string concept
// 0.3  Removed command::append for ranges (fails with fs::path (has begin-end))
// 0.2  Added string_view support
// 0.1  Added command, args_parser, get_files_by_ext

#ifndef _CPP_SCRIPT_INCLUDE_HPP
#define _CPP_SCRIPT_INCLUDE_HPP

#include <algorithm>
#include <cassert>
#include <cstring>
#include <concepts>
#include <cstdlib>
#include <iostream>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Formatter specializations for std::format

namespace script::detail
{
struct default_formatter_parser {
    constexpr auto parse(std::format_parse_context& ctx) {
        return ctx.begin();
    }
};
} // namespace script::detail

template<>
struct std::formatter<std::filesystem::path> :
    script::detail::default_formatter_parser
{
    auto format(const std::filesystem::path& p, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", p.string());
    }
};

// Covers command, args_parser, import_info
template<typename T>
    requires requires (T t) {
        { t.to_string() } -> std::same_as<std::string>;
    }
struct std::formatter<T> :
    script::detail::default_formatter_parser
{
    auto format(const T& t, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", t.to_string());
    }
};

namespace script
{
// [1] Print, println

template<typename ...Args>
void print(std::format_string<Args...> fmt, Args&& ...as) {
    std::cout << std::format(fmt, std::forward<Args>(as)...);
}

template<typename ...Args>
void println(std::format_string<Args...> fmt, Args&& ...as) {
    std::cout << std::format(fmt, std::forward<Args>(as)...) << std::endl;
}

inline void print(std::string s) {
    std::cout << s;
}

inline void println(std::string s) {
    std::cout << s << std::endl;
}

// [2] Filesystem IO functions, file search

namespace fs = std::filesystem;

template<typename Range>
concept byte_range = 
    std::ranges::contiguous_range<Range> &&
    (sizeof(std::ranges::range_value_t<Range>) == 1);

template<typename Range>
concept resizable_range = requires(Range r, std::ranges::range_size_t<Range> s) {
    r.resize(s);
};

template<byte_range ByteRange>
bool write_binary_file(const std::filesystem::path& file, const ByteRange& bytes) {
    namespace fs = std::filesystem;
    using value_type = std::ranges::range_value_t<ByteRange>;
    
    const auto dirs = file.parent_path();
    if (fs::exists(dirs))
        if (!fs::create_directories(dirs))
            return false;

    std::basic_ofstream<value_type> os{ file };
    return (bool)os.write(std::ranges::data(bytes), std::ranges::size(bytes));
}

template<byte_range ByteRange>
std::optional<ByteRange> read_binary_file(const std::filesystem::path& file) {
    static_assert(resizable_range<ByteRange>);
    
    namespace fs = std::filesystem;
    using value_type = std::ranges::range_value_t<ByteRange>;

    if (!fs::exists(file))
        return std::nullopt;
    assert(!fs::is_directory(file));

    ByteRange bytes;
    bytes.resize(fs::file_size(file));
    std::basic_ifstream<value_type> is{ file, std::ios::binary };
    if (is.read(bytes.data(), bytes.size()))
        return bytes;
    return std::nullopt;
}

inline std::vector<fs::path> 
    get_files_by_ext(const fs::path& dir, const fs::path& ext, bool recursive = false)
{
    assert(fs::exists(dir) && fs::is_directory(dir));

    std::vector<fs::path> ret;
    auto output = std::back_inserter(ret);
    auto same_ext = [&](auto& entry){ return entry.path().extension() == ext; };
    if (recursive)
        std::ranges::copy_if(fs::recursive_directory_iterator(dir), output, same_ext);
    else
        std::ranges::copy_if(fs::directory_iterator(dir), output, same_ext);
    return ret;
}

// [3] File loader/parser for storing script data

/// File structure:
/// ```ini
///  [Header1]
///  Key1=Value1
///  Key2=Value2|Value3
///  [Header2]
///  ...
/// ```
/// @warning Header must be unique in a file scope
/// @warning Key must be unique in a header scope
class info_file {
    // Key -> Values
    using value_lines_type = std::unordered_map<std::string, std::vector<std::string>>;
    
    // Header -> ValueLines
    using rep_type = std::unordered_map<std::string, value_lines_type>;

    struct tmp_header {
        std::string_view              Header;
        std::vector<std::string_view> ValueLines;
    };

    /// @brief Step 1 tokenization, checks '[]' braces match
    /// @return [Header1, Values1], [Header2, Values2], ... 
    std::vector<tmp_header> parse_headers(const std::string_view file) noexcept {
        std::vector<tmp_header> headers;
        
        size_t brace_count  = 0;
        size_t header_first = 0; // Where header begins
        size_t values_first = 0; // Where values line begins
        bool is_header_line = false;
        
        for (size_t i = 0; i < file.size(); ++i) {
            const auto c = file[i];
            
            if (c == '[') { // Header started
                is_header_line = true;
                header_first = i + 1; // skip '['
                ++brace_count;
            }
            else if (c == ']') { // Header ended
                headers.emplace_back().Header = 
                    file.substr(header_first, i - header_first);
                --brace_count;
            }
            else if (c == '\n') {
                if (!is_header_line && !headers.empty())
                    headers.back().ValueLines.emplace_back(
                        file.substr(values_first, i - values_first));
                
                // Assume that the next line is values line
                values_first = i + 1; // skip '\n'

                // We can only be sure after '\n', not after ']'
                // Maybe there are spaces or some other shit
                if (is_header_line)
                    is_header_line = false;
            }
        }
        assert(brace_count == 0 && "'[]' braces mismatch");
        return headers;
    }

    /// @brief Step 2 tokenization
    /// @param values_line 'Key=Value1|Value2|...|ValueN'
    /// @return [Key, [Value1, Value2, ...]]
    value_lines_type::value_type parse_values(const std::string_view values_line) {
        std::vector<std::string> values;
        
        const size_t key_last = values_line.find('=');
        size_t first = key_last + 1; // Skip '='
        while (first != values_line.size()) {
            size_t last = values_line.find('|', first);
            const bool last_value = (last == std::string::npos);
            if (last_value)
                last = values_line.size();
            
            values.emplace_back(values_line.substr(first, last - first));
            
            first = last;
            if (!last_value)
                ++first; // Skip '|'
        }
        std::string key{ values_line.substr(0, key_last) };
        return { std::move(key), std::move(values) };
    }

    void parse(const std::string& s) noexcept {
        const auto headers = parse_headers(s);
        for (auto& h : headers) {
            value_lines_type value_line;
            for (auto& vl : h.ValueLines) {
                auto&& [key, values] = parse_values(vl);
                auto [_, ok] = value_line.emplace(std::move(key), std::move(values));
                assert(ok && "File keys must be unique within a header");
            }
            auto [_, ok] = Rep.emplace(std::string(h.Header), std::move(value_line));
            assert(ok && "Headers must be unique within a file");
        }
    }

public:
    info_file() noexcept = default;

    info_file(const std::filesystem::path& file) noexcept {
        if (auto s = read_binary_file<std::string>(file)) {
            assert(s.has_value() && "Failed to load info_file");
            parse(*s);
        }
    }

    value_lines_type& operator[] (const std::string& header) {
        return Rep[header];
    }

    bool save(const std::filesystem::path& file) const {
        std::string s;
        for (auto& [header, lines] : Rep) {
            s += std::format("[{}]\n", header);
            for (auto& [key, values] : lines) {
                s += std::format("{}=", key);
                for (auto& value : values)
                    s += std::format("{}|", value);
                
                if (!values.empty())
                    s.back() = '\n'; // replace '|'
                else
                    s += '\n';
            }
        }
        return write_binary_file(file, s);
    }

private:
    rep_type Rep;
};

namespace detail
{
/// @brief Script info file singletone loader
class script_info_file_t {
public:
    static constexpr std::string_view FileName{ ".cpp_script.txt" };
    
    script_info_file_t() noexcept = default;

    ~script_info_file_t() {
        if (File)
            File->save(FileName);
    }

    info_file& get() noexcept {
        if (!File)
            File = std::make_unique<info_file>(FileName);
        return *File;
    }

    void save() {
        assert(File != nullptr && "Called save on empty script file wrapper");
        if (!File->save(FileName))
            assert(false && "Failed to save script file");
    }

private:
    std::unique_ptr<info_file> File;
};

inline script_info_file_t script_info_file;
} // namespace detail

// [4] SHA256 hash, file contents change detection

namespace detail
{
// SHA256 implementation
// Copyright(c) 2010 Ilya O.Levin, http://www.literatecode.com
extern "C"
{
typedef struct {
    uint32_t buf[16];
    uint32_t hash[8];
    uint32_t len[2];
} sha256_context;

void sha256_init(sha256_context* ctx);
void sha256_hash(sha256_context* ctx, const uint8_t* data, uint32_t len);
void sha256_done(sha256_context* ctx, uint8_t* hash);

#define RL(x,n)   (((x) << n) | ((x) >> (32 - n)))
#define RR(x,n)   (((x) >> n) | ((x) << (32 - n)))

#define S0(x)  (RR((x), 2) ^ RR((x),13) ^ RR((x),22))
#define S1(x)  (RR((x), 6) ^ RR((x),11) ^ RR((x),25))
#define G0(x)  (RR((x), 7) ^ RR((x),18) ^ ((x) >> 3))
#define G1(x)  (RR((x),17) ^ RR((x),19) ^ ((x) >> 10))

const uint32_t K[64] = {
     0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
     0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
     0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
     0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
     0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
     0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
     0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
     0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
     0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
     0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
     0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
     0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
     0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
     0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
     0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
     0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

void _bswapw(uint32_t *p, uint32_t i)
{
    while (i--) p[i] = (RR(p[i],24) & 0x00ff00ff) | (RR(p[i],8) & 0xff00ff00);
} /* _bswapw */

void _rtrf(uint32_t *b, uint32_t *p, uint32_t i, uint32_t j)
{
    #define B(x, y) b[(x-y) & 7]
    #define P(x, y) p[(x+y) & 15]

    B(7,i) += (j ? (p[i & 15] += G1(P(i,14)) + P(i,9) + G0(P(i,1))) : p[i & 15])
              + K[i+j] + S1(B(4,i))
              + (B(6,i) ^ (B(4,i) & (B(5,i) ^ B(6,i))));
    B(3,i) += B(7,i);
    B(7,i) += S0(B(0,i)) + ( (B(0,i) & B(1,i)) | (B(2,i) & (B(0,i) ^ B(1,i))) );

    #undef P
    #undef B
} /* _rtrf */

void _hash(sha256_context *ctx)
{
    uint32_t b[8], *p, j;

    b[0] = ctx->hash[0]; b[1] = ctx->hash[1]; b[2] = ctx->hash[2];
    b[3] = ctx->hash[3]; b[4] = ctx->hash[4]; b[5] = ctx->hash[5];
    b[6] = ctx->hash[6]; b[7] = ctx->hash[7];

    for (p = ctx->buf, j = 0; j < 64; j += 16)
        _rtrf(b, p,  0, j), _rtrf(b, p,  1, j), _rtrf(b, p,  2, j),
        _rtrf(b, p,  3, j), _rtrf(b, p,  4, j), _rtrf(b, p,  5, j),
        _rtrf(b, p,  6, j), _rtrf(b, p,  7, j), _rtrf(b, p,  8, j),
        _rtrf(b, p,  9, j), _rtrf(b, p, 10, j), _rtrf(b, p, 11, j),
        _rtrf(b, p, 12, j), _rtrf(b, p, 13, j), _rtrf(b, p, 14, j),
        _rtrf(b, p, 15, j);

    ctx->hash[0] += b[0]; ctx->hash[1] += b[1]; ctx->hash[2] += b[2];
    ctx->hash[3] += b[3]; ctx->hash[4] += b[4]; ctx->hash[5] += b[5];
    ctx->hash[6] += b[6]; ctx->hash[7] += b[7];
} /* _hash */

void sha256_init(sha256_context *ctx)
{
    ctx->len[0] = ctx->len[1] = 0;
    ctx->hash[0] = 0x6a09e667; ctx->hash[1] = 0xbb67ae85;
    ctx->hash[2] = 0x3c6ef372; ctx->hash[3] = 0xa54ff53a;
    ctx->hash[4] = 0x510e527f; ctx->hash[5] = 0x9b05688c;
    ctx->hash[6] = 0x1f83d9ab; ctx->hash[7] = 0x5be0cd19;
} /* sha256_init */

void sha256_hash(sha256_context *ctx, const uint8_t *dat, uint32_t sz)
{
    //register uint32_t i = ctx->len[0] & 63, l, j;
    uint32_t i = ctx->len[0] & 63, l, j;

    if ((ctx->len[0] += sz) < sz)  ++(ctx->len[1]);

    for (j = 0, l = 64-i; sz >= l; j += l, sz -= l, l = 64, i = 0)
    {
        memcpy((char *)ctx->buf + i, &dat[j], l);
        _bswapw(ctx->buf, 16 );
        _hash(ctx);
    }
    memcpy((char *)ctx->buf + i, &dat[j], sz);
} /* _hash */

void sha256_done(sha256_context *ctx, uint8_t *buf)
{
    uint32_t i = (uint32_t)(ctx->len[0] & 63), j = ((~i) & 3) << 3;

    _bswapw(ctx->buf, (i + 3) >> 2);

    ctx->buf[i >> 2] &= 0xffffff80 << j;  /* add padding */
    ctx->buf[i >> 2] |= 0x00000080 << j;

    if (i < 56) i = (i >> 2) + 1;
       else ctx->buf[15] ^= (i < 60) ? ctx->buf[15] : 0, _hash(ctx), i = 0;

    while (i < 14) ctx->buf[i++] = 0;

    ctx->buf[14] = (ctx->len[1] << 3)|(ctx->len[0] >> 29); /* add length */
    ctx->buf[15] = ctx->len[0] << 3;

    _hash(ctx);

    for (i = 0; i < 32; i++)
       ctx->buf[i % 16] = 0, /* may remove this line in case of a DIY cleanup */
       buf[i] = (uint8_t)(ctx->hash[i >> 2] >> ((~i & 3) << 3));
} /* sha256_done */
} // extern "C"
} // namespace detail

template<byte_range ByteRange>
inline std::string make_sha256_hash(const ByteRange& range) {
    using value_type = std::ranges::range_value_t<ByteRange>;
    
    detail::sha256_context ctx;
    detail::sha256_init(&ctx);
    detail::sha256_hash(&ctx, (uint8_t*)range.data(), (uint32_t)range.size());
    std::vector<uint8_t> hash(32);
    detail::sha256_done(&ctx, hash.data());

    std::stringstream readable;
    for (auto& byte : hash)
        readable << std::format("{:02x}", byte);
    return readable.str();
}

inline bool file_contents_changed(const fs::path& file) {
    assert(fs::exists(file) && !fs::is_directory(file));

    // Accessing through wrapper
    auto& info_file = detail::script_info_file;

    const auto hash = [&]() -> std::string {
        auto bytes = read_binary_file<std::string>(file);
        assert(bytes.has_value() && "Failed to make file hash");
        return make_sha256_hash(*bytes);
    }();
    auto& old_hash = [&]() -> std::string& {
        auto& stored = info_file.get()["Hashes"][file];
        return stored.empty() ? stored.emplace_back() : stored.front();
    }();    
    const bool hash_changed = (old_hash != hash);
    if (hash_changed) {
        old_hash = hash;
        info_file.save();
    }
    return hash_changed;
}

// [5] [argc, argv] handlers

class args_parser {
public:
    args_parser(int argc, char** argv) {
        while(argc--)
            _Args.emplace_back(*argv++);
    }
    
    // Returns a path to executable
    fs::path get_path() const {
        return fs::absolute(_Args.front());
    }

    fs::path get_parent_dir() const {
        return get_path().parent_path();
    }
    
    bool is_empty() const {
        // Always has a path to executable
        return _Args.size() == 1;
    }
    
    // Searches for "...key..." substr in args
    // Returns the first arg, where found
    std::optional<std::string> has(const std::string_view key) const {
        for (size_t i = 1; i < _Args.size(); ++i) {
            auto& arg = _Args[i];
            if (arg.find(key) != std::string::npos)
                return arg;
        }
        return std::nullopt;
    }
    
    // Searches for "...key..." substr in args
    // Returns the rest of arg after key
    // Example: arg = "keyvalue", key = "key", return = "value"
    std::optional<std::string> get(const std::string_view key) const {
        if (auto arg = has(key))
            return arg->substr(key.size());
        return std::nullopt;
    }
    
    constexpr std::string to_string() const {
        std::string ret;
        // Skip the path 
        for (size_t i = 1; i < _Args.size(); ++i)
            ret += _Args[i] + " ";
        return ret;
    }

private:
    std::vector<std::string> _Args;
};

template<typename T>
concept arg_entry = requires(T entry, std::string_view arg) {
    { entry.parse(arg) };
    { entry.get_default() };
};

template<arg_entry Arg>
using arg_result_t = decltype(std::declval<Arg>().parse(std::declval<std::string_view>()));

// Forward declaration - has no 'Default' - handled differently
struct arg_bool;

template<typename ...ArgEntries>
class args_table {
public:
    static_assert((arg_entry<ArgEntries> && ...));

    constexpr args_table(ArgEntries&& ...as) noexcept :
        _Entries{ std::forward<ArgEntries>(as)... }
    {
        size_t key_width   = 0;
        size_t descr_width = 0;
        std::apply([&](auto& ...as) noexcept {
            (..., [&](auto& arg) noexcept {
                if (key_width < arg.Key.size())
                    key_width = arg.Key.size();
                if (descr_width < arg.Description.size())
                    descr_width = arg.Description.size();
            }(as));
        }, _Entries);
        _KeyWidth         = 4 + key_width;
        _DescriptionWidth = 4 + descr_width;
    }

    void show_help() const {
        std::apply([this](auto& ...as){
            (...,[&, this](auto& entry) {
                if constexpr (std::same_as<arg_bool, std::decay_t<decltype(entry)>>)
                    println("\t{:{}}{:{}}",
                        entry.Key, _KeyWidth, 
                        entry.Description, _DescriptionWidth);
                else
                    println("\t{:{}}{:{}}default: {}",
                        entry.Key, _KeyWidth,
                        entry.Description, _DescriptionWidth,
                        entry.Default);
            }(as));
        }, _Entries);
    }

    auto extract(const args_parser& parser, bool print_parsed = false) const {
        auto find_by_key = [&](auto& entry, auto& out_value) {
            if (auto value = parser.get(entry.Key))
                out_value = entry.parse(*value);
            else
                out_value = entry.get_default();
            if (print_parsed)
                println("Parsed {:{}}{}", entry.Key, _KeyWidth, out_value);
        };
        std::tuple<arg_result_t<ArgEntries>...> ret{};
        [&]<size_t... Is>(std::index_sequence<Is...>){
            (..., find_by_key(std::get<Is>(_Entries), std::get<Is>(ret)));
        }(std::make_index_sequence<sizeof...(ArgEntries)>{});
        return ret;
    }

private:
    std::tuple<ArgEntries...> _Entries;
    size_t                    _KeyWidth{};
    size_t                    _DescriptionWidth{};
};

/// @brief True if arg is presented, false otherwise
struct arg_bool {
    using result_type = bool;
    
    constexpr arg_bool(std::string_view key, std::string_view descr) noexcept :
        Key{ key }, Description{ descr } {}
    
    constexpr result_type parse(const std::string_view) const noexcept {
        return true;
    }

    constexpr result_type get_default() const noexcept {
        return false;
    }
    
public:
    const std::string_view Key;
    const std::string_view Description;
};

/// @brief For types constructible from const char*
template<typename Result>
struct arg_string_rep : arg_bool {
    using result_type = Result;

    constexpr arg_string_rep(std::string_view key, std::string_view descr, std::string_view _default) noexcept :
        arg_bool{ key, descr}, Default{ _default } {}

    constexpr result_type parse(const std::string_view arg) const noexcept {
        return arg.data();
    }

    constexpr result_type get_default() const noexcept {
        return Default;
    }

public:
    const result_type Default;
};

struct arg_string : arg_string_rep<std::string> {
    using _Base = arg_string_rep<std::string>;
    using _Base::_Base;
};

struct arg_path : arg_string_rep<fs::path> {
    using _Base = arg_string_rep<fs::path>;
    using _Base::_Base;
};

// [6] Command

class command {
    void _append(const auto& s) {
        _Text += std::format("{} ", s);
    }
    
public:
    constexpr command() noexcept = default;

    command(auto&& ...ss) {
        (..., _append(std::forward<decltype(ss)>(ss)));
    }

    void append(auto&& ...ss) {
        (..., _append(std::forward<decltype(ss)>(ss)));
    }

    std::string to_string() const {
        return _Text;
    }

    int run() const {
        std::cout << "Running: " << _Text << '\n';
        return system(_Text.c_str());
    }

private:
    std::string _Text;
};

// [7] Auto-rebuild

/// @brief Rebuilds the script if any of its sources has changed
/// @param args Script arguments
/// @param compile Compiler and compiler args ("g++ -std=c++20" is enough)
/// @param ...src_files Script source code
/// @return True if was rebuilt and executed, false otherwise 
template<typename ...Paths>
bool try_rebuild_itself(
    const args_parser& args,
    command            compile,
    Paths&&         ...src_files)
{
    static_assert((std::constructible_from<fs::path, Paths> && ...));

    bool src_changed = false;
    (..., [&](auto& file) {
        if (!src_changed)
            src_changed = file_contents_changed(file);
    }(src_files));

    if (src_changed) {
        const fs::path out_file = args.get_path();
        const fs::path old_out_file = out_file.string() + ".old";
        if (fs::exists(old_out_file))
            fs::remove(old_out_file); // remove previous
        fs::rename(out_file, old_out_file);
        
        compile.append("-o", out_file);
        (..., compile.append(src_files));
        if (0 == compile.run()) {
            command run_new{ out_file, args };
            run_new.run();
            return true;
        }

        // Compilation failed - fallback to old file
        fs::rename(old_out_file, out_file);
    }
    return false;
}

// [8] Library import

namespace detail
{
class import_info {
public:
    import_info(
        std::vector<std::string> includes,
        std::vector<std::string> lib_paths,
        std::vector<std::string> libs
    ) noexcept :
        Includes{ std::move(includes) },
        LibPaths{ std::move(lib_paths) },
        Libs    { std::move(libs) } {}

    std::string to_string() const {
        std::string ret;
        auto append = [&](std::string_view prefix, auto& sv){
            for (auto& s : sv)
                ret += std::format("{}{} ", prefix, s);
        };
        append("-I", Includes);
        append("-L", LibPaths);
        append("-l", Libs);
        return ret;
    }

private:
    std::vector<std::string> Includes;
    std::vector<std::string> LibPaths;
    std::vector<std::string> Libs;
};
} // namespace detail

inline void make_import_guide(
    std::vector<std::string> includes,
    std::vector<std::string> lib_paths,
    std::vector<std::string> libs)
{
    auto& info_file = detail::script_info_file;
    auto& import = info_file.get()["Import"];
    import["-I"] = std::move(includes);
    import["-L"] = std::move(lib_paths);
    import["-l"] = std::move(libs);
    info_file.save();
}

inline detail::import_info import(const fs::path& lib_dir) {
    info_file lib_info{ lib_dir };
    auto& import = lib_info["Import"];
    return {
        std::move(import["-I"]),
        std::move(import["-L"]),
        std::move(import["-l"])
    };
}
} // namespace script

#endif // !_CPP_SCRIPT_INCLUDE_HPP