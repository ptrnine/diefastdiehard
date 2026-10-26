#pragma once

#include <bit>
#include <tuple>
#include <span>
#include <cstring>
#include <cstdint>
#include <vector>
#include <stdexcept>

//#define VERBOSE_DEBUG

#ifdef VERBOSE_DEBUG
#include "io.hpp"
#endif

#define SS_SERIALIZE(...) \
    void serialize(auto& s) const { \
        s.write(__VA_ARGS__); \
    } \
    void deserialize(auto& d) { \
        d.read(__VA_ARGS__); \
    }

namespace ss {

#ifdef VERBOSE_DEBUG
    static inline auto stdo = dfdh::outfd<char, 4096 * 16>::stdout();
    static inline constexpr auto verbose = true;
    static size_t nestcounter = 0;

    template <typename... Ts>
    void verbose_print(Ts&&... args) {
        for (size_t i = 0; i < nestcounter * 4; ++i)
            stdo.write(' ');
        ([]<typename T>(T&& v) {
            if constexpr (requires (T&& v) { stdo.write(v); })
                stdo.write(v);
            else
                stdo.write("<unknown>");
        }(std::forward<Ts>(args)), ...);
        stdo.write('\n');
    }

    void print_bytes(const void* b, size_t sz) {
        for (size_t i = 0; i < nestcounter * 4; ++i)
            stdo.write(' ');
        auto bs = reinterpret_cast<const char*>(b); // NOLINT
        static constexpr auto hexdigits = "0123456789abcdef";
        for (size_t i = 0; i < sz; ++i)
            stdo.write(hexdigits[uint8_t(bs[i]) >> 4], hexdigits[uint8_t(bs[i]) & 0xf], ' ');
        stdo.write('\n');
    }

    #define VERBOSE(...) verbose_print(__VA_ARGS__)
    #define VERBOSE_IN() ++nestcounter
    #define VERBOSE_OUT() --nestcounter
    #define VERBOSE_BYTES(...) print_bytes(__VA_ARGS__)
#else
    static inline constexpr auto verbose = false;
    #define VERBOSE(...) do {} while(0)
    #define VERBOSE_IN() do {} while(0)
    #define VERBOSE_OUT() do {} while(0)
    #define VERBOSE_BYTES(...) do {} while(0)
#endif


template <auto N>
struct nttp {
    constexpr auto operator+() const noexcept { return N; }
    constexpr operator decltype(N)() const noexcept { return N; }
};

template <typename T>
struct type_c {
    using type = T;
    T operator+() const;
};

namespace policy {
    namespace endian {
        inline constexpr auto big    = nttp<std::endian::big>{};
        inline constexpr auto little = nttp<std::endian::little>{};
        inline constexpr auto native = nttp<std::endian::native>{};

        using big_t    = decltype(big);
        using little_t = decltype(little);
        using native_t = decltype(native);
    } // namespace endian

    namespace flatcopy {
        enum class flatcopy { off = 0, on };
        inline constexpr auto off = nttp<flatcopy::off>{};
        inline constexpr auto on = nttp<flatcopy::on>{};

        using off_t = decltype(off);
        using on_t  = decltype(on);
    } // namespace flatcopy

    template <typename T>
    using size_type_t = type_c<T>;

    template <typename T>
    inline constexpr auto size_type = size_type_t<T>{};
}

template <auto Endian   = policy::endian::little,
          auto Flatcopy = policy::flatcopy::off,
          auto SizeType = policy::size_type<uint64_t>>
struct serialize_policy {
    template <typename... Ts>
    serialize_policy(Ts...) {}

    static inline constexpr auto endian    = Endian;
    static inline constexpr auto flatcopy  = Flatcopy;
    static inline constexpr auto size_type = SizeType;
};

namespace details {
    template <typename F, typename Default, typename T>
    struct last_type_get_helper {
        using type = T;
    };

    template <typename F, typename Default, typename T1, typename T2>
    auto operator+(last_type_get_helper<F, Default, T1>, last_type_get_helper<F, Default, T2>) {
        if constexpr (F{}(T2{}))
            return last_type_get_helper<F, Default, T2>{};
        else if constexpr (F{}(T1{}))
            return last_type_get_helper<F, Default, T1>{};
        else
            return last_type_get_helper<F, Default, Default>{};
    }
}

template <typename F, typename Default, typename... Ts>
using match_last_type =
    typename decltype((details::last_type_get_helper<F, Default, Default>{} + ... +
                       details::last_type_get_helper<F, Default, Ts>{}))::type;

template <typename... Ts>
constexpr auto get_endian() {
    constexpr auto f = [](auto v) {
        return std::is_same_v<decltype(+v), std::endian>;
    };
    return match_last_type<decltype(f), decltype(serialize_policy<>::endian), Ts...>{};
}

template <typename... Ts>
constexpr auto get_flatcopy() {
    constexpr auto f = [](auto v) {
        return std::is_same_v<decltype(+v), policy::flatcopy::flatcopy>;
    };
    return match_last_type<decltype(f), decltype(serialize_policy<>::flatcopy), Ts...>{};
}

template <typename... Ts>
constexpr auto get_size_type() {
    constexpr auto f = [](auto v) {
        return std::unsigned_integral<decltype(+v)>;
    };
    return match_last_type<decltype(f), decltype(serialize_policy<>::size_type), Ts...>{};
}

template <typename T, typename... Ts>
concept contained_in = (std::same_as<T, Ts> || ... || false);

template <typename T>
concept PolicyArg = requires (T p) {
    {+p} -> contained_in<std::endian, policy::flatcopy::flatcopy, uint8_t, uint16_t, uint32_t, uint64_t>;
};

template <typename... Ts>
serialize_policy(Ts...) -> serialize_policy<get_endian<Ts...>(),
                                            get_flatcopy<Ts...>(),
                                            get_size_type<Ts...>()>;

template <typename P>
struct ptr_autocast {
    template <typename T> requires (sizeof(T) == 1)
    operator T*() {
        return reinterpret_cast<T*>(ptr); // NOLINT
    }
    P* ptr;
};

template <typename T>
concept SerializeBackendFd = requires (T& b) {
    b.write(ptr_autocast<void>{nullptr}, size_t(0));
};

template <typename T>
concept SerializeBackendBuffer = requires (T& b) {
    b.resize(b.size());
    std::memcpy(b.data(), nullptr, size_t(0));
};

template <typename T>
concept SerializeBackendV = SerializeBackendFd<T> || SerializeBackendBuffer<T>;

template <typename T>
concept SerializeBackend = SerializeBackendV<T> || SerializeBackendV<std::remove_pointer_t<T>>;

template <typename T>
concept pointer = std::is_pointer_v<T>;

template <SerializeBackendBuffer B, typename T>
void backend_write(B&& backend, T* data, size_t count) {
    auto backend_sz = backend.size();
    auto sz = count * sizeof(T);

    backend.resize(backend_sz + sz);
    std::memcpy(backend.data() + backend_sz, data, sz);

    VERBOSE("backend_write: ", std::to_string(sz), " bytes, ", std::to_string(backend.size()), " total");
    VERBOSE_BYTES(data, sz);
}

template <SerializeBackendFd B, typename T>
void backend_write(B&& backend, T* data, size_t count) {
    auto sz = count * sizeof(T);
    backend.write(ptr_autocast<T>{data}, sz);
    VERBOSE("backend_write: ", std::to_string(sz), " bytes");
    VERBOSE_BYTES(data, sz);
}

template <SerializeBackendV B, typename T>
void backend_write(B* backend, T* data, size_t count) {
    backend_write(*backend, data, count);
}

template <typename T>
concept DeserializeBackendBuffer = requires (const T& d) {
    {d.size()} -> std::convertible_to<size_t>;
    {d.data()} -> pointer;
};

template <typename T>
concept DeserializeBackendFdV = requires (T&& d) {
    {d.read(ptr_autocast<void>{nullptr}, size_t(0))};
};

template <typename T>
concept DeserializeBackendFdPtr = DeserializeBackendFdV<std::remove_pointer_t<T>> && !DeserializeBackendFdV<T>;

template <typename T>
concept DeserializeBackendFd = DeserializeBackendFdV<T> || DeserializeBackendFdPtr<T>;

template <typename T>
concept DeserializeBackend = DeserializeBackendFd<T> || DeserializeBackendBuffer<T>;

template <DeserializeBackendBuffer B, typename T>
void backend_read(B&& backend, T* data, size_t count) {
    auto backend_sz = backend.size() * sizeof(*backend.data());
    auto data_sz    = count * sizeof(T);
    if (backend_sz < data_sz)
        throw std::out_of_range("deserialize: out of input data range");

    std::memcpy(data, backend.data(), data_sz);

    VERBOSE("backend_read: ", std::to_string(data_sz), " bytes, ", std::to_string(backend.size()), " left");
    VERBOSE_BYTES(backend.data(), data_sz);
}

template <DeserializeBackendFdV B, typename T>
void backend_read(B&& backend, T* data, size_t count) {
    auto sz   = count * sizeof(T);
    auto read = backend.read(ptr_autocast<T>{data}, sz);

    VERBOSE("backend_read: ", std::to_string(sz), " bytes");
    VERBOSE_BYTES(data, sz);
    /* XXX: check read == count * sizeof(T) */
}

template <DeserializeBackendFdPtr B, typename T>
void backend_read(B backend, T* data, size_t count) {
    backend_read(*backend, data, count);
}

template <typename T, typename De_Serializer>
concept HasSerializeMember = requires(const T& v, De_Serializer& s) {
    v.serialize(s);
} || requires(T& v, De_Serializer& d) {
    v.deserialize(d);
};

template <typename T, typename De_Serializer>
concept FlatCopyable =
    +De_Serializer::policy::flatcopy == policy::flatcopy::flatcopy::on &&
    +De_Serializer::policy::endian == std::endian::native&& std::is_trivial_v<std::decay_t<T>> &&
    !pointer<std::decay_t<T>> && !HasSerializeMember<T, De_Serializer>;

template <typename T, typename De_Serializer>
concept Integral = std::integral<T> && !FlatCopyable<T, De_Serializer>;

template <typename T, typename De_Serializer>
concept UnsignedIntegral = Integral<T, De_Serializer> && std::is_unsigned_v<T>;

template <typename T, typename De_Serializer>
concept FloatingPoint = std::floating_point<T> && !FlatCopyable<T, De_Serializer>;

template <typename T, typename De_Serializer>
concept Enum = std::is_enum_v<T> && (!Integral<T, De_Serializer>) && !FlatCopyable<T, De_Serializer>;

template <typename T>
concept Incrementible = requires (T& v) {
    ++v;
};

template <typename T, typename De_Serializer>
concept SizedRange = requires (const T& v) {
    {v.size()} -> std::convertible_to<size_t>;
    {v.begin()} -> Incrementible;
    {v.end()} -> Incrementible;
} && !FlatCopyable<T, De_Serializer>;

template <typename T, typename De_Serializer>
concept Tuple = requires { std::tuple_size<std::decay_t<T>>::value; } &&
                !FlatCopyable<T, De_Serializer> &&
                !HasSerializeMember<T, De_Serializer>;

template <typename T, typename De_Serializer>
concept FlatRange = SizedRange<T, De_Serializer> && requires (const T& v) {
    {v.data()} -> pointer;
} && !HasSerializeMember<T, De_Serializer>;


template <typename T, typename De_Serializer>
concept TrivialFlatRange = FlatRange<T, De_Serializer> && requires (T&& v) {
    {*v.data()} -> FlatCopyable<De_Serializer>;
};

namespace ds {
    template <typename T, typename De_Serializer>
    concept ResizableFlatRange = FlatRange<T, De_Serializer> && requires (T& v) {
        v.resize(1);
    };

    template <typename T, typename De_Serializer>
    concept ResizableTrivialFlatRange = TrivialFlatRange<T, De_Serializer> && ResizableFlatRange<T, De_Serializer>;

    template <typename T, typename De_Serializer>
    concept EmplacibleSizedRange = SizedRange<T, De_Serializer> && requires (T& v) {
        v.emplace();
    };

    template <typename T, typename De_Serializer>
    concept PushBackableSizedRange = SizedRange<T, De_Serializer> && requires (T& v) {
        v.push_back({});
        v.back();
    } && !EmplacibleSizedRange<T, De_Serializer> && !ResizableFlatRange<T, De_Serializer>;
}

template <typename T, typename De_Serializer>
concept Optional = requires (T&& v) {
    static_cast<bool>(v);
    {*v};
} && !FlatCopyable<T, De_Serializer> && !HasSerializeMember<T, De_Serializer>;

template <typename T, typename Serializer>
concept Serializable = requires (Serializer& s, T&& v) {
    s.write(v);
};

template <typename T, typename Deserializer>
concept Deserializable = requires (Deserializer& d, T& v) {
    d.read(v);
};

template <SerializeBackend backend_t, typename Policy = serialize_policy<>>
struct serializer_base {
    using policy  = Policy;
    using size_type = decltype(+Policy::size_type);

    serializer_base() = default;

    template <typename T>
    serializer_base(T&& storage): data_holder(std::forward<T>(storage)) {}

    template <UnsignedIntegral<serializer_base> T>
    void write_impl(T v) {
        VERBOSE("[unsigned integral]: ", std::to_string(v));

        if constexpr (+policy::endian != std::endian::native)
            v = bswap(v);
        backend_write(data_holder, &v, 1);
    }

    template <Integral<serializer_base> T>
    void write_impl(T v) {
        VERBOSE("[integral]: ", std::to_string(v));

        VERBOSE_IN();
        write_impl(static_cast<std::make_unsigned_t<T>>(v));
        VERBOSE_OUT();
    }

    template <FloatingPoint<serializer_base> T>
    void write_impl(T v) {
        VERBOSE("[floating_point]: ", std::to_string(v));
        using int_t = std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>;
        int_t n;
        memcpy(&n, &v, sizeof(v));

        VERBOSE_IN();
        write_impl(n);
        VERBOSE_OUT();
    }

    template <Optional<serializer_base> T>
    void write_impl(T v) {
        VERBOSE("[optional]: ", v);

        auto test = static_cast<bool>(v);

        VERBOSE_IN();

        write_impl(test);
        if (v)
            write_impl(*v);

        VERBOSE_OUT();
    }

    template <Enum<serializer_base> T>
    void write_impl(T v) {
        VERBOSE("[enum]: ", v);
        VERBOSE_IN();
        write_impl(static_cast<std::underlying_type<T>>(v));
        VERBOSE_OUT();
    }

    template <FlatCopyable<serializer_base> T>
    void write_impl(T&& v) {
        VERBOSE("[flat_copyable]");
        VERBOSE_IN();
        backend_write(data_holder, &v, 1);
        VERBOSE_OUT();
    }

    template <TrivialFlatRange<serializer_base> T>
    void write_impl(T&& v) {
        VERBOSE("[trivial_flat_range]: size: ", std::to_string(v.size()));

        VERBOSE_IN();
        write_impl(size_type(v.size()));
        backend_write(data_holder, v.data(), v.size());
        VERBOSE_OUT();
    }

    template <FlatRange<serializer_base> T>
    void write_impl(T&& v) {
        VERBOSE("[flat_range]: size: ", std::to_string(v.size()));

        VERBOSE_IN();
        write_impl(size_type(v.size()));
        for (auto b = v.data(), e = v.data() + v.size(); b < e; ++b)
            write_impl(*b);
        VERBOSE_OUT();
    }

    template <SizedRange<serializer_base> T>
    void write_impl(T&& v) {
        VERBOSE("[sized_range]: size: ", std::to_string(v.size()));

        VERBOSE_IN();
        write_impl(size_type(v.size()));
        for (auto&& e : v)
            write_impl(e);
        VERBOSE_OUT();
    }

    template <Tuple<serializer_base> T>
    void write_impl(T&& v) {
        VERBOSE("[tuple]: size: ", std::to_string(std::tuple_size_v<std::decay_t<T>>));

        VERBOSE_IN();
        [this]<size_t... Idxs>(T&& v, std::index_sequence<Idxs...>) {
            (write_impl(std::get<Idxs>(v)), ...);
        }(v, std::make_index_sequence<std::tuple_size_v<std::decay_t<T>>>{});
        VERBOSE_OUT();
    }

    template <HasSerializeMember<serializer_base> T>
    void write_impl(T&& v) {
        VERBOSE("[member serialize]");
        VERBOSE_IN();
        v.serialize(*this);
        VERBOSE_OUT();
    }

    template <typename T, size_t S>
    void write_impl(const T(&v)[S]) {
        VERBOSE("[c_array]: size: ", std::to_string(S));

        VERBOSE_IN();
        if constexpr (FlatCopyable<T, serializer_base>) {
            backend_write(data_holder, v, S);
        }
        else {
            for (auto b = v, e = v + S; b < e; ++b)
                write_impl(*b);
        }
        VERBOSE_OUT();
    }

    template <typename... Ts>
    void write(Ts&&... args) {
        (write_impl(std::forward<Ts>(args)), ...);
    }

    backend_t data_holder;
};

template <typename T>
static constexpr bool ready_to_move =
    std::is_rvalue_reference_v<T> && !std::is_const_v<std::remove_reference_t<T>>;


template <typename T>
constexpr decltype(auto) move_or_ptr(T&& v) {
    if constexpr (ready_to_move<T&&>)
        return std::forward<T>(v);
    else
        return &v;
}

template <typename T>
using serializer_holder_type = std::remove_reference_t<decltype(move_or_ptr(std::declval<T>()))>;

template <typename... Ts>
auto serialize_policy_f() {
    return serialize_policy(Ts{}...);
}

template <typename... Ts>
using s_policy_t = decltype(serialize_policy_f<Ts...>());

template <SerializeBackend backend_t, PolicyArg... PolicyTs>
struct serializer
    : public serializer_base<serializer_holder_type<backend_t>, s_policy_t<PolicyTs...>> {
    using super = serializer_base<serializer_holder_type<backend_t>, s_policy_t<PolicyTs...>>;
    template <SerializeBackend T>
    serializer(T&& data_holder, PolicyTs...): super(move_or_ptr(std::forward<T>(data_holder))) {}
    serializer() = default;
};

template <SerializeBackend T, PolicyArg... PolicyTs>
serializer(T&&, PolicyTs...) -> serializer<T&&, PolicyTs...>;

template <template <typename... Ts> class T, typename... Ts>
auto tuple_elements_reset_const(T<Ts...>) {
    return T<std::remove_const_t<Ts>...>{};
}

template <DeserializeBackend backend_t, typename Policy = serialize_policy<>>
struct deserializer_base {
    using policy  = Policy;
    using size_type = decltype(+Policy::size_type);

    deserializer_base() = default;

    template <typename... Ts>
    deserializer_base(Ts&&... ibackend): data_remains(std::forward<Ts>(ibackend)...) {}

    template <UnsignedIntegral<deserializer_base> T>
    void read_impl(T& v) {
        VERBOSE("[unsigned_integral]: ", std::to_string(v));
        VERBOSE_IN();

        read_backend_impl(&v, 1);
        if constexpr (+policy::endian != std::endian::native)
            v = bswap(v);

        VERBOSE_OUT();
    }

    template <Integral<deserializer_base> T>
    void read_impl(T& v) {
        VERBOSE("[integral]: ", std::to_string(v));
        VERBOSE_IN();

        std::make_unsigned_t<T> result;
        read_impl(result);
        v = static_cast<T>(result);

        VERBOSE_OUT();
    }

    template <FloatingPoint<deserializer_base> T>
    void read_impl(T& v) {
        VERBOSE("[floating_point]: ", std::to_string(v));
        VERBOSE_IN();

        using int_t = std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>;
        int_t n;
        read_impl(n);
        memcpy(&v, &n, sizeof(v));

        VERBOSE_OUT();
    }

    template <Optional<deserializer_base> T>
    void read_impl(T& v) {
        VERBOSE("[optional]");
        VERBOSE_IN();

        bool contains;
        read_impl(contains);

        if (contains) {
            v = std::decay_t<decltype(*v)>{};
            read_impl(*v);
        }
        else
            v = {};

        VERBOSE_OUT();
    }

    template <Enum<deserializer_base> T>
    void read_impl(T& v) {
        VERBOSE("[enum]");
        VERBOSE_IN();

        std::underlying_type<T> res;
        read_impl(res);
        v = static_cast<T>(res);

        VERBOSE_OUT();
    }

    template <FlatCopyable<deserializer_base> T>
    void read_impl(T& v) {
        VERBOSE("[flat_copyable]");
        VERBOSE_IN();

        read_backend_impl(&v, 1);

        VERBOSE_OUT();
    }

    template <ds::ResizableTrivialFlatRange<deserializer_base> T>
    void read_impl(T& v) {
        VERBOSE("[resizable_trivial_flat_range]");
        VERBOSE_IN();

        size_type sz;
        read_impl(sz);

        size_t old_sz = v.size();
        auto new_sz = old_sz + sz;

        v.resize(new_sz);
        read_backend_impl(v.data() + old_sz, sz);

        VERBOSE_OUT();
    }

    template <TrivialFlatRange<deserializer_base> T>
    void read_impl(T& v) {
        VERBOSE("[trivial_flat_range]");
        VERBOSE_IN();

        size_type sz;
        read_impl(sz);

        /* XXX: assert that sz == v.size() */

        read_backend_impl(v.data(), sz);

        VERBOSE_OUT();
    }

    template <ds::ResizableFlatRange<deserializer_base> T>
    void read_impl(T& v) {
        VERBOSE("[resizable_flat_range]");
        VERBOSE_IN();

        size_type sz;
        read_impl(sz);

        size_t old_sz = v.size();
        auto new_sz = old_sz + sz;

        //try {
        v.resize(new_sz);
        //} catch (...) {
            //print_bytes(data_remains);
        //    std::abort();
        //}

        auto p = v.data() + old_sz;
        for (auto e = p + sz; p < e; ++p)
            read_impl(*p);

        VERBOSE_OUT();
    }

    template <FlatRange<deserializer_base> T>
    void read_impl(T& v) {
        VERBOSE("[flat_range]");
        VERBOSE_IN();

        size_type sz;
        read_impl(sz);

        /* XXX: assert that sz == v.size() */
        for (auto p = v.data(), e = v.data() + sz; p < e; ++p)
            read_impl(*p);

        VERBOSE_OUT();
    }

    template <ds::EmplacibleSizedRange<deserializer_base> T>
    void read_impl(T& v) {
        VERBOSE("[emplacible_sized_range]");
        VERBOSE_IN();

        size_type sz;
        read_impl(sz);

        for (size_type i = 0; i < sz; ++i) {
            /* *map.begin() returns pair whith const key element
             * try fix this
             */
            decltype(tuple_elements_reset_const(*v.begin())) bucket;
            read_impl(bucket);
            v.emplace(std::move(bucket));
        }

        VERBOSE_OUT();
    }

    template <ds::PushBackableSizedRange<deserializer_base> T>
    void read_impl(T& v) {
        VERBOSE("[push_backable_sized_range]");
        VERBOSE_IN();

        size_type sz;
        read_impl(sz);

        for (size_type i = 0; i < sz; ++i) {
            v.push_back({});
            read_impl(v.back());
        }

        VERBOSE_OUT();
    }

    template <Tuple<deserializer_base> T>
    void read_impl(T& v) {
        VERBOSE("[tuple]: size: ", std::to_string(std::tuple_size_v<std::decay_t<T>>));
        VERBOSE_IN();

        [this]<size_t... Idxs>(T& v, std::index_sequence<Idxs...>) {
            (read_impl(std::get<Idxs>(v)), ...);
        }(v, std::make_index_sequence<std::tuple_size_v<std::decay_t<T>>>{});

        VERBOSE_OUT();
    }

    template <HasSerializeMember<deserializer_base> T>
    void read_impl(T&& v) {
        VERBOSE("[member serialize]");
        VERBOSE_IN();

        v.deserialize(*this);

        VERBOSE_OUT();
    }

    template <typename T, size_t S>
    void read_impl(T(&v)[S]) {
        VERBOSE("[c_array]: size: ", std::to_string(S));
        VERBOSE_IN();

        if constexpr (FlatCopyable<T, deserializer_base>) {
            read_backend_impl(v, S);
        }
        else {
            for (auto b = v, e = v + S; b < e; ++b)
                read_impl(*b);
        }

        VERBOSE_OUT();
    }

    template <typename... Ts>
    void read(Ts&&... args) {
        (read_impl(std::forward<Ts>(args)), ...);
    }

    template <typename T, typename B = backend_t>
    void read_backend_impl(T* data, size_t count) {
        backend_read(data_remains, data, count);
        if constexpr (DeserializeBackendBuffer<B>) {
            /* Shift remains */
            auto sz = count * sizeof(T);
            data_remains = decltype(data_remains){
                data_remains.data() + sz, data_remains.size() - sz};
        }
    }

    [[nodiscard]]
    size_t available() const {
        return data_remains.size();
    }

    backend_t data_remains;
};

template <typename T>
auto deserializer_span(T&& b) {
    return std::span{b.data(), 1};
}
template <typename T>
using ds_span_t = decltype(deserializer_span(std::declval<T>()));

template <typename... Ts>
struct deserializer;

/**
 * @brief Buffer-based view deserializer
 */
template <DeserializeBackendBuffer backend_t, PolicyArg... PolicyTs>
struct deserializer<backend_t, PolicyTs...>
    : public deserializer_base<ds_span_t<backend_t>, s_policy_t<PolicyTs...>> {
    using super = deserializer_base<ds_span_t<backend_t>, s_policy_t<PolicyTs...>>;
    deserializer(const backend_t& storage, PolicyTs...): super(storage.data(), storage.size()) {}
};

/**
 * @brief Buffer-based owning deserializer
 */
template <DeserializeBackendBuffer backend_t, PolicyArg... PolicyTs>
struct deserializer<backend_t&&, PolicyTs...>
    : public deserializer_base<ds_span_t<backend_t>, s_policy_t<PolicyTs...>> {
    using super = deserializer_base<ds_span_t<backend_t>, s_policy_t<PolicyTs...>>;
    deserializer(backend_t&& storage, PolicyTs...):
        super(storage.data(), storage.size()), data_storage(std::move(storage)) {}

    void reset_position() {
        this->data_remains = decltype(this->data_remains){data_storage.data(), data_storage.size()};
    }

    backend_t data_storage;
};

/**
 * @brief Stream-based owning deserializer
 */
template <DeserializeBackendFdV backend_t, PolicyArg... PolicyTs>
struct deserializer<backend_t&&, PolicyTs...>
    :  public deserializer_base<backend_t, s_policy_t<PolicyTs...>> {
    using super = deserializer_base<backend_t, s_policy_t<PolicyTs...>>;
    deserializer(backend_t&& storage, PolicyTs...): super(std::move(storage)) {}
};

/**
 * @brief Stream-based not owning deserializer
 */
template <DeserializeBackendFdV backend_t, PolicyArg... PolicyTs>
struct deserializer<backend_t, PolicyTs...>
    : public deserializer_base<std::add_pointer_t<backend_t>, s_policy_t<PolicyTs...>> {
    using super = deserializer_base<std::add_pointer_t<backend_t>, s_policy_t<PolicyTs...>>;
    deserializer(const backend_t& storage, PolicyTs...): super(&storage) {}
};

template <DeserializeBackend backend_t, PolicyArg... PolicyTs>
deserializer(backend_t&&, PolicyTs...) -> deserializer<std::conditional_t<ready_to_move<backend_t&&>, backend_t&&, backend_t>, PolicyTs...>;

} // namespace ss
