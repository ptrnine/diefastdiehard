#include <chrono>
#include <map>
#include <iostream>
#include <filesystem>
#include <optional>

#include "base/io.hpp"
#include "base/split_view.hpp"
#include "base/md5.hpp"
#include "base/print.hpp"

using namespace dfdh;
namespace fs = std::filesystem;

static bool                                      skip_system_includes = true;
static bool                                      hash_output          = false;
static std::vector<fs::path>                     includes;
static std::map<std::string, fs::file_time_type> info;
static std::optional<std::string>                check_path;

auto get_file_time(const fs::path& file_path) {
    return fs::last_write_time(file_path);
}

void analyze_comments(bool& on_multiline_comment, std::string_view remains) {
    size_t pos = 0;
    while (true) {
        auto slash_pos = remains.find('/', pos);
        if (slash_pos == std::string_view::npos)
            return;

        if (on_multiline_comment) {
            if (slash_pos != 0 && remains[slash_pos - 1] == '*')
                on_multiline_comment = false;
        }
        else if (slash_pos + 1 < remains.size() && remains[slash_pos + 1] == '*') {
            on_multiline_comment = true;
        }
        pos = slash_pos + 1;
    }
}

bool depinfo(const fs::path& file_path, bool is_system_include = false);

void parse_depinfo(const fs::path& file_path) {
    bool on_multiline_comment = false;
    auto fv                   = mmap_file_range<char>(file_path.string().data());

    for (auto line : fv / split('\n', '\r')) {
        auto        p       = line.begin();
        auto        e       = line.end();
        auto        start_p = p;
        std::string include_data;
        bool        is_system_include;

        if (on_multiline_comment)
            goto analyze_and_next; // NOLINT

        while (p != e && (*p == ' ' || *p == '\t'))
            ++p;
        if (p == e || *p != '#')
            goto analyze_and_next; // NOLINT
        ++p;

        while (p != e && (*p == ' ' || *p == '\t'))
            ++p;
        if (!std::string_view(p, e).starts_with("include"))
            goto analyze_and_next; // NOLINT
        p += sizeof("include") - 1;

        while (p != e && (*p == ' ' || *p == '\t'))
            ++p;

        if (p != e && (*p == '"' || *p == '<')) {
            char start_c = *p;
            char end_p   = start_c == '<' ? '>' : '"';
            ++p;
            start_p = p;
            while (p != e && *p != end_p && *p != '\n' && *p != '\r')
                ++p;
            if (p == e || *p != end_p)
                goto analyze_and_next; // NOLINT
        }
        else
            goto analyze_and_next; // NOLINT

        include_data = std::string(start_p, p);
        is_system_include = *p == '>';
        if (!depinfo(file_path.parent_path() / include_data, is_system_include)) {
            for (auto& include : includes)
                if (depinfo(include / include_data, is_system_include))
                    break;
        }

    analyze_and_next:
        analyze_comments(on_multiline_comment, std::string_view(p, e));
    }
}

bool depinfo(const fs::path& file_path, bool is_system_include) {
    auto path = fs::weakly_canonical(file_path);

    if (!fs::is_regular_file(path))
        return false;


    bool was_insert = info.emplace(std::string(path), get_file_time(path)).second;
    if (!was_insert || (skip_system_includes && is_system_include))
        return true;

    try {
        parse_depinfo(path);
    } catch (...) {
        return false;
    }
    return true;
}

int main(int, char** argv) {
    for (auto argp = argv + 1; *argp; ++argp) {
        auto arg = std::string_view(*argp);
        if (arg == "-I" || arg == "-isystem") {
            ++argp;
            if (*argp) {
                auto path = std::string_view(*argp);
                if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
                    path = path.substr(1, path.size() - 2);
                includes.push_back(fs::weakly_canonical(path));
                continue;
            }
            else {
                break;
            }
        }
        else if (arg == "-C") {
            ++argp;
            if (*argp) {
                check_path = *argp;
                continue;
            }
            else {
                break;
            }
        }
        else if (arg == "--hash") {
            hash_output = true;
            continue;
        }
        else if (arg == "--skip-system") {
            skip_system_includes = true;
            continue;
        }

        depinfo(arg);
    }

    std::string output;
    for (auto& [name, time] : info)
        output += build_string(
            name,
            ":"sv,
            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count()),
            "\n"sv);

    if (hash_output)
        output = dfdh::format("{}\n", md5(output));

    int rc = 0;
    if (check_path) {
        try {
            auto fv = mmap_file_range<char>(check_path->data());
            if (std::string_view(fv.begin(), fv.end()) != output)
                rc = 1;
        }
        catch (...) {
            rc = 1;
        }
    }

    std::cout << output;
    return rc;
}
