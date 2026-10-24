#pragma once

#include <cstring>
#include <string>

#include "types.hpp"
#include "print.hpp"
#include "serialization.hpp"

namespace dfdh {

inline constexpr size_t aligned_size(size_t size) {
    return size == 0 ? 0 : ((size - 1) / 8 + 1) * 8;
}

template <typename C, size_t S>
class fixed_str_t {
public:
    SS_SERIALIZE(_data)

    static constexpr size_t max_size_w_nt = aligned_size(S + 1);

    constexpr fixed_str_t() = default;
    constexpr ~fixed_str_t() = default;

    template <size_t SS>
    constexpr fixed_str_t(const C(&str)[SS]): _len(std::min(SS - 1, max_size_w_nt - 1)) {
        std::memcpy(_data.data(), str, _len);
        _data[_len] = C('\0');
    }

    fixed_str_t(const char* str, size_t size): _len(std::min(size, max_size_w_nt - 1)) {
        std::memcpy(_data.data(), str, _len);
        _data[_len] = C('\0');
    }

    fixed_str_t(const std::basic_string<C>& str): _len(std::min(str.size(), max_size_w_nt - 1)) {
        std::memcpy(_data.data(), str.data(), _len);
        _data[_len] = C('\0');
    }

    constexpr fixed_str_t(std::basic_string_view<C> str): _len(std::min(str.size(), max_size_w_nt - 1)) {
        std::memcpy(_data.data(), str.data(), _len);
        _data[_len] = C('\0');
    }

    operator std::basic_string_view<C>() const {
        return std::basic_string_view<C>(_data.data(), _len);
    }

    operator std::basic_string<C>() const {
        return std::basic_string<C>(_data.data(), _len);
    }

    C& operator[](size_t idx) {
        return _data[idx];
    }

    const C& operator[](size_t idx) const {
        return _data[idx];
    }

    [[nodiscard]]
    bool operator==(const fixed_str_t& str) const {
        return _len == str._len && std::memcmp(_data.data(), str._data.data(), _len) == 0;
    }

    [[nodiscard]]
    bool operator==(std::string_view str) const {
        return _len == str.size() && std::memcmp(_data.data(), str.data(), _len) == 0;
    }

    [[nodiscard]]
    bool operator<(const fixed_str_t& str) const {
        return std::string_view(*this) < std::string_view(str);
    }

    [[nodiscard]]
    friend bool operator==(std::string_view rhs, const fixed_str_t& lhs) {
        return lhs == rhs;
    }

    [[nodiscard]]
    constexpr size_t size() const {
        return _len;
    }

    [[nodiscard]]
    constexpr bool empty() const {
        return size() == 0;
    }

private:
    u64                          _len = {};
    std::array<C, max_size_w_nt> _data = {};
};

template <size_t S>
using fixed_str = fixed_str_t<char, S>;

using player_name_t = fixed_str<23>;

template <size_t S>
struct printer<fixed_str<S>> {
    void operator()(std::ostream& os, const fixed_str<S>& str) const {
        os << std::string_view(str);
    }
};

}
