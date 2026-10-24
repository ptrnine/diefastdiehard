#pragma once

#include <iostream>
#include <sstream>
#include <tuple>
#include <optional>
#include <chrono>

#include "types.hpp"

namespace dfdh {

template <typename T>
struct printer;

template <typename T>
concept PrinterStandard = requires(const T& v) {
    std::cout << v;
};

template <typename T>
concept Printer = requires {
    printer<std::decay_t<T>>();
} && !PrinterStandard<T>;

template <typename T>
concept PrinterRange = requires(const T& v) {
    {v.begin()};
    {v.end()};
} && !Printer<T> && !PrinterStandard<T>;

inline void print_any(std::ostream& os, bool value) {
    os << (value ? "true" : "false");
}

inline void print_any(std::ostream& os, std::byte value) {
    constexpr auto hex = "0123456789abcdef";
    os << "0x" << hex[static_cast<u8>(value) >> 4] << hex[static_cast<u8>(value) & 0xF0];
}

void print_any(std::ostream& os, const PrinterStandard auto& value) {
    os << value;
}

void print_any(std::ostream& os, const PrinterRange auto& value);
template <typename T, size_t S>

void print_any(std::ostream& os, const T(&value)[S]);

template <Printer T>
void print_any(std::ostream& os, const T& value) {
    printer<T>()(os, value);
}

template <typename T, size_t S>
void print_any(std::ostream& os, const T(&value)[S]) {
    if constexpr (std::is_same_v<T, char>) {
        os << value;
        return;
    }

    if constexpr (sizeof(S) == 0)
        os << "{}";
    if constexpr (sizeof(S) == 1)
        os << '{' << value[0] << '}';
    else {
        os << '{';
        for (size_t i = 0; i < S - 1; ++i)
            os << value[i] << ", ";

        os << value[S - 1] << '}';
    }
}

void print_any(std::ostream& os, const PrinterRange auto& value) {
    if (value.begin() == value.end()) {
        os << "{}";
        return;
    }

    os << '{';
    for (auto i = value.begin();;) {
        print_any(os, *i);
        if constexpr (!std::is_same_v<std::decay_t<decltype(*i)>, char>) {
            if (++i != value.end())
                os << ", ";
            else
                break;
        }
        else {
            if (++i == value.end())
                break;
        }
    }
    os << '}';
}

template <typename T>
concept PrintableTuple = requires {
    std::tuple_size<T>::value;
};

template <PrintableTuple T>
struct printer<T> {

    template <size_t... S>
    static void print_by_idxs(std::ostream& os, const T& v, std::index_sequence<S...>) {
        using std::get;
        if constexpr (std::is_same_v<std::decay_t<decltype(get<0>(v))>, char>)
            (print_any(os, get<S>(v)), ...);
        else
            ((print_any(os, get<S>(v)), print_any(os, ", ")), ...);
    }

    template <size_t... S>
    static void print(std::ostream& os, const T& v, std::index_sequence<S...>) {
        using std::get;
        if constexpr (sizeof...(S) == 0)
            os << "{}";
        if constexpr (sizeof...(S) == 1)
            print_any(os, '{', get<0>(v), '}');
        if constexpr (sizeof...(S) > 1) {
            os << '{';
            print_by_idxs(os, v, std::make_index_sequence<sizeof...(S) - 1>());
            print_any(os, get<sizeof...(S) - 1>(v));
            os << '}';
        }
    }

    void operator()(std::ostream& os, const T& v) const {
        print(os, v, std::make_index_sequence<std::tuple_size_v<T>>());
    }
};

template <typename T>
struct printer<std::optional<T>> {
    void operator()(std::ostream& os, const std::optional<T>& v) const {
        print_any(os, v ? *v : "(null)");
    }
};

template <typename... Ts>
void fprint(std::ostream& os, const Ts&... args) {
    (print_any(os, args), ...);
}

template <typename... Ts>
void fprintln(std::ostream& os, const Ts&... args) {
    (print_any(os, args), ...);
    os << std::endl;
}

template <typename... Ts>
void print(const Ts&... args) {
    fprint(std::cout, args...);
}

template <typename... Ts>
void println(const Ts&... args) {
    fprintln(std::cout, args...);
}

template <size_t I = 0, typename... Ts>
void fprintf(std::ostream& os, std::string_view format_str, const Ts&... args) {
    size_t fmt_start = 0;
    size_t fmt_size  = 0;
    bool on_fmt = false;

    for (size_t i = 0; i < format_str.size(); ++i) {
        auto c = format_str[i];

        if (on_fmt) {
            if (c == '}') {
                // TODO: formats
                //auto fmt = format_str.substr(fmt_start, fmt_size);
                on_fmt = false;
                if constexpr (I < sizeof...(Ts)) {
                    print_any(os, std::get<I>(std::tuple<const Ts&...>(args...)));
                    fprintf<I + 1>(os, format_str.substr(fmt_start + fmt_size + 1), args...);
                    return;
                }
            } else
                ++fmt_size;
        } else {
            if (c == '{') {
                on_fmt = true;
                fmt_start = i + 1;
                fmt_size = 0;
            } else  {
                os << c;
            }
        }
    }
}

template <typename... Ts>
void printf(std::string_view format_str, const Ts&... args) {
    fprintf(std::cout, format_str, args...);
}

template <typename... Ts>
void fprintfln(std::ostream& os, std::string_view format_str, const Ts&... args) {
    fprintf(os, format_str, args...);
    os << std::endl;
}

template <typename... Ts>
void printfln(std::string_view format_str, const Ts&... args) {
    fprintfln(std::cout, format_str, args...);
}

template <typename... Ts>
std::string format(std::string_view format, const Ts&... args) {
    std::stringstream ss;
    fprintf(ss, format, args...);
    return ss.str();
}

template <typename Rep, typename Period>
struct printer<std::chrono::duration<Rep, Period>> {
    inline static std::string nanotime_str(std::chrono::nanoseconds time_nano) {
        auto time = time_nano.count();

        if (time < 1000LL)
            return dfdh::format("{}ns", time);
        else if (time < 1000000LL)
            return dfdh::format("{}us", time / 1000LL);
        else if (time < 1000000000LL)
            return dfdh::format("{}ms", time / 1000000LL);
        else if (time < 1000000000LL * 60)
            return dfdh::format("{}s", time / 1000000000LL);
        else if (time < 1000000000LL * 60 * 60)
            return dfdh::format("{}minutes", time / (1000000000LL * 60));
        else
            return dfdh::format("{}hours", time / (1000000000LL * 60 * 60));
    }


    void operator()(std::ostream& os, const std::chrono::duration<Rep, Period>& dur) const {
        os << nanotime_str(std::chrono::duration_cast<std::chrono::nanoseconds>(dur));
    }
};

} // namespace dfdh
