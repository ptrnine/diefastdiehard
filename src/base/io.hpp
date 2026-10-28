#pragma once

#include <stdexcept>
#include <cstring>
#include <chrono>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef _WIN32

#include <io.h>
#include <windows.h>

#ifndef isatty
#define isatty _isatty
#endif

/* Mode bits MinGW's <sys/stat.h> doesn't define */
#ifndef S_ISUID
#define S_ISUID 04000
#endif
#ifndef S_ISGID
#define S_ISGID 02000
#endif
#ifndef S_ISVTX
#define S_ISVTX 01000
#endif
#ifndef S_IFSOCK
#define S_IFSOCK 0x4000
#endif

#else

#include <sys/mman.h>
#include <sys/select.h>

#endif

#include "errc.hpp"
#include "finalizers.hpp"

namespace dfdh
{
class io_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class cannot_open_file : public std::runtime_error {
public:
    cannot_open_file(const std::string& filename, errc err):
        std::runtime_error("Cannot open file \"" + filename + "\": " + err.info()), error(err) {}
    errc error;
};

class stat_fd_failed : public std::runtime_error {
public:
    stat_fd_failed(const std::string& filename, errc err, int iunclosed_fd = -1):
        std::runtime_error("Failed to stat file \"" + filename + "\": " + err.info()),
        error(err),
        unclosed_fd(iunclosed_fd) {}
    errc error;
    int  unclosed_fd;
};

class mmap_fd_failed : public std::runtime_error {
public:
    mmap_fd_failed(const std::string& filename, errc err, int iunclosed_fd = -1):
        std::runtime_error("Failed to mmap file \"" + filename + "\": " + err.info()),
        error(err),
        unclosed_fd(iunclosed_fd) {}
    errc error;
    int  unclosed_fd;
};

class sys_write_fail : public io_error {
public:
    sys_write_fail(const std::string& msg, errc err):
        io_error("write() syscall failed: " + msg + (err.code ? (": " + err.info()) : std::string{})), error(err) {}
    errc error;
};

class sys_read_fail : public io_error {
public:
    sys_read_fail(errc err): io_error("read() syscall failed: " + err.info()), error(err) {}
    errc error;
};

class sys_seek_fail : public io_error {
public:
    sys_seek_fail(errc err): io_error("lseek() syscall failed: " + err.info()), error(err) {}
    errc error;
};

enum class fd_flag {
    read_only      = O_RDONLY,
    write_only     = O_WRONLY,
    readwrite      = O_RDWR,
    append         = O_APPEND,
#ifndef _WIN32
    async          = O_ASYNC,
    close_exec     = O_CLOEXEC,
    direct         = O_DIRECT,
    dsync          = O_DSYNC,
    sync           = O_SYNC,
    large          = O_LARGEFILE,
    no_access_time = O_NOATIME,
    no_follow      = O_NOFOLLOW,
    temp           = O_TMPFILE,
#endif
    create         = O_CREAT,
    ensure_new     = O_EXCL | O_CREAT,
#ifndef _WIN32
    nonblock       = O_NONBLOCK,
#endif
    trunc          = O_TRUNC
};

struct fd_combined_flag {
    fd_combined_flag(fd_flag iflag): f(int(iflag)) {}
    fd_combined_flag operator|(fd_flag flag) const {
        return fd_flag(f | int(flag));
    }
    fd_combined_flag& operator|=(fd_flag flag) {
        f |= int(flag);
        return *this;
    }

    int f;
};

inline fd_combined_flag operator|(fd_flag lhs, fd_flag rhs) {
    using underlying_t = std::underlying_type_t<fd_flag>;
    return fd_flag(static_cast<underlying_t>(lhs) | static_cast<underlying_t>(rhs));
}

struct file_permissions {
public:
    enum perms_e : uint16_t {
        none         = 0,
        user_read    = S_IRUSR,
        user_write   = S_IWUSR,
        user_exec    = S_IXUSR,
        user_id_bit  = S_ISUID,
        user_rw      = user_read | user_write,
        user_rwx     = user_read | user_write | user_exec,
        group_read   = S_IRGRP,
        group_write  = S_IWGRP,
        group_exec   = S_IXGRP,
        group_id_bit = S_ISGID,
        group_rw     = group_read | group_write,
        group_rwx    = group_read | group_write | group_exec,
        other_read   = S_IROTH,
        other_write  = S_IWOTH,
        other_exec   = S_IXOTH,
        sticky_bit   = S_ISVTX,
        other_rw     = other_read | other_write,
        other_rwx    = other_read | other_write | other_exec
    };

    struct permissions_string {
        static constexpr size_t permissions_str_size = 9;

        const char* begin() const {
            return data;
        }
        const char* end() const {
            return data + permissions_str_size;
        }

        constexpr size_t size() const noexcept {
            return permissions_str_size;
        }

        char data[permissions_str_size + 1] = "---------";
    };

    constexpr file_permissions(perms_e permissions = none): data(permissions) {}

    constexpr permissions_string to_string() const {
        return {{
            data & user_read ? 'r' : '-',
            data & user_write ? 'w' : '-',
            "-xSs"[(data & user_exec ? 1 : 0) + (data & user_id_bit ? 2 : 0)],
            data & group_read ? 'r' : '-',
            data & group_write ? 'w' : '-',
            "-xSs"[(data & group_exec ? 1 : 0) + (data & group_id_bit ? 2 : 0)],
            data & other_read ? 'r' : '-',
            data & other_write ? 'w' : '-',
            "-xTt"[(data & other_exec ? 1 : 0) + (data & sticky_bit ? 2 : 0)],
        }};
    }

    constexpr file_permissions& operator|=(file_permissions rhs) {
        data |= rhs.data;
        return *this;
    }

    constexpr file_permissions& operator&=(file_permissions rhs) {
        data &= rhs.data;
        return *this;
    }

    constexpr file_permissions operator|(file_permissions rhs) const {
        auto res = *this;
        res |= rhs;
        return res;
    }

    constexpr file_permissions operator&(file_permissions rhs) const {
        auto res = *this;
        res &= rhs;
        return res;
    }

    constexpr file_permissions operator~() const {
        return perms_e(~data & uint16_t(07777));
    }

    constexpr static file_permissions from_number(int number) {
        return perms_e(number);
    }

    constexpr bool operator==(file_permissions rhs) const {
        return data == rhs.data;
    }

    constexpr bool operator!=(file_permissions rhs) const {
        return data != rhs.data;
    }

    constexpr explicit operator bool() const {
        return data;
    }

    constexpr bool operator!() const {
        return !data;
    }

    int to_int() const {
        return data;
    }

private:
    uint16_t data;
};

constexpr inline file_permissions operator|(file_permissions::perms_e lhs, file_permissions::perms_e rhs) {
    return file_permissions(lhs) | rhs;
}

enum class io_read_rc {
    ok = 0,
    timeout,
    partial,
    error
};

struct io_read_res {
    size_t      actually_read;
    io_read_rc rc = io_read_rc::ok;
};

namespace details
{
    template <typename T>
    constexpr bool check_enable(auto... settings) {
        return ((std::is_same_v<decltype(settings), T> ? T::enable == settings : false) || ...);
    }

    template <typename T>
    constexpr auto settings_get_value(auto... settings) {
        constexpr auto get_v = [](auto v) constexpr {
            if constexpr (std::is_same_v<T, decltype(v)>)
                return +v;
            else
                return 0;
        };
        return (get_v(settings) + ... + 0);
    }
} // namespace details

/* FD settings */
enum class fd_exception_on_syswrite_fail { disable = 0, enable };
enum class fd_exception_on_seek_fail { disable = 0, enable };
enum class fd_exception_on_read_fail { disable = 0, enable };

namespace outfd_concepts
{
    using std::begin, std::end;

    template <typename StringLike, typename T>
    concept WritableBase = requires(const StringLike& s) {
                               { begin(s) } -> std::convertible_to<const T*>;
                               { end(s) } -> std::convertible_to<const T*>;
                               { end(s) - begin(s) } -> std::convertible_to<ptrdiff_t>;
                           };

    template <typename Container, typename T>
    concept WritableRAI = requires(const Container& s) {
                              { begin(s).base() } -> std::convertible_to<const T*>;
                              { *end(s) } -> std::convertible_to<T>;
                              { end(s) - begin(s) } -> std::convertible_to<ptrdiff_t>;
                          };

    template <typename Container, typename T>
    concept Writable = WritableBase<Container, T> || WritableRAI<Container, T> || std::convertible_to<Container, T>;
} // namespace outfd_concepts

#ifdef _WIN32

/* POSIX-ish CRT fd functions from <io.h> */
#define impl_open_posix   ::_open
#define impl_close_posix  ::_close
#define impl_write_posix  ::_write
#define impl_read_posix   ::_read
#define impl_seek_posix   ::_lseek
#define impl_fstat_posix  ::fstat
#define impl_get_osfhandle ::_get_osfhandle

#else

#define impl_open_posix   ::open
#define impl_close_posix  ::close
#define impl_write_posix  ::write
#define impl_read_posix   ::read
#define impl_seek_posix   ::lseek
#define impl_fstat_posix  ::fstat

#endif

template <typename DerivedT, auto... Settings>
struct outfd_linux_impl_t {
    static constexpr bool exception_on_syswrite_fail =
        details::check_enable<fd_exception_on_syswrite_fail>(Settings...);
    static constexpr bool exception_on_seek_fail    = details::check_enable<fd_exception_on_seek_fail>(Settings...);
    static constexpr bool exception_on_read_fail    = details::check_enable<fd_exception_on_read_fail>(Settings...);

    static int impl_open(const char* filename, fd_combined_flag flags, file_permissions create_permissions) {
        int fd;
        if ((fd = impl_open_posix(filename, flags.f, create_permissions.to_int())) < 0)
            throw cannot_open_file(filename, errc::from_errno());
        return fd;
    }

    static void impl_close(int fd) noexcept {
        if (fd != STDOUT_FILENO && fd != STDERR_FILENO && fd != STDIN_FILENO)
            impl_close_posix(fd);
    }

    static void impl_write(int fd, const void* data, size_t size) noexcept(!exception_on_syswrite_fail) {
        [[maybe_unused]] auto sz = impl_write_posix(fd, data, size);
        if constexpr (exception_on_syswrite_fail) {
            if (size_t(sz) != size)
                throw sys_write_fail(std::to_string(sz) + " of " + std::to_string(size) + " bytes has been wrote", {0});
        }
    }

    static io_read_res impl_read(int fd, void* data, size_t size) noexcept(!exception_on_read_fail) {
        auto actually_read = impl_read_posix(fd, data, size);
        if (actually_read < 0) {
            auto err = errc::from_errno();
            if (err == EAGAIN)
                return {0, io_read_rc::timeout};
            else if constexpr (exception_on_read_fail)
                throw sys_read_fail(err);
            else
                return {0, io_read_rc::error};
        }
        return {size_t(actually_read), actually_read == size ? io_read_rc::ok : io_read_rc::partial};
    }

    static void impl_seek(int fd, ssize_t value) noexcept(!exception_on_seek_fail) {
        [[maybe_unused]] auto rc = impl_seek_posix(fd, value, SEEK_CUR);
        if constexpr (exception_on_seek_fail)
            if (rc < 0)
                throw sys_seek_fail(errc::from_errno());
    }

    /* Named std_out/std_err/std_in because stdout/stderr/stdin are macros on Windows */
    constexpr static DerivedT std_out() {
        return DerivedT::raw_create(STDOUT_FILENO);
    }

    constexpr static DerivedT std_err() {
        return DerivedT::raw_create(STDERR_FILENO);
    }

    constexpr static DerivedT std_in() {
        return DerivedT::raw_create(STDIN_FILENO);
    }

    constexpr int default_outfd() const noexcept {
        return STDOUT_FILENO;
    }

    constexpr int default_infd() const noexcept {
        return STDIN_FILENO;
    }

    static bool impl_is_blocking(int fd) {
        struct stat st;
        auto rc = impl_fstat_posix(fd, &st);
        if (rc == -1)
            throw stat_fd_failed("", errc::from_errno());

        auto fmt = st.st_mode & S_IFMT;
        return fmt == S_IFSOCK || fmt == S_IFIFO || fmt == S_IFCHR;
    }

#ifdef _WIN32

    /* Not needed on Windows (no select() for CRT fds) — always ready. */
    static bool impl_waitdev(int, int64_t) {
        return true;
    }

#else

    static bool impl_waitdev(int fd, int64_t usec_timeout) {
        struct timeval tv {};
        tv.tv_usec = usec_timeout;

        fd_set readfds = {};
        FD_SET(fd, &readfds);
        return ::select(1, &readfds, nullptr, nullptr, &tv);
    }

#endif
};

template <typename DerivedT, auto... Settings>
struct outfd_devnull_impl {
    static constexpr bool exception_on_syswrite_fail =
        details::check_enable<fd_exception_on_syswrite_fail>(Settings...);
    static constexpr bool exception_on_seek_fail = details::check_enable<fd_exception_on_seek_fail>(Settings...);

    static int impl_open(const char*, fd_combined_flag, file_permissions) {
        return 0;
    }
    static void impl_close(int) {}
    static void impl_write(int, const void*, size_t) noexcept(!exception_on_syswrite_fail) {}
    static io_read_res impl_read(int, void*, size_t) noexcept {
        return {0};
    }
    static void impl_seek(int, ssize_t) noexcept(!exception_on_seek_fail) {}

    constexpr int default_outfd() const noexcept {
        return 0;
    }

    constexpr int default_infd() const noexcept {
        return 0;
    }

    constexpr static bool impl_is_blocking(int) {
        return false;
    }

    static bool impl_waitdev(int, int64_t) {
        return true;
    }
};

template <typename DerivedT, typename Impl, typename T = char, size_t S = 8192, auto... Settings>
class outfd_base_t : public Impl { // NOLINT
public:
    template <typename DerivedT2, typename Impl2, typename TT, size_t SS, auto... SSettings>
    friend class outfd_base_t;

    using fd_type = decltype(Impl::impl_open("", fd_flag{}, file_permissions{}));

    static constexpr bool exception_on_syswrite_fail =
        details::check_enable<fd_exception_on_syswrite_fail>(Settings...);
    static constexpr bool exception_on_seek_fail = details::check_enable<fd_exception_on_seek_fail>(Settings...);

    constexpr static DerivedT raw_create(fd_type fd) {
        DerivedT of;
        of.fd = fd;
        return of;
    }

    outfd_base_t() = default;

    outfd_base_t(const char* filename, file_permissions create_permissions):
        fd(Impl::impl_open(filename, fd_flag::write_only | fd_flag::create | fd_flag::trunc, create_permissions)) {}

    outfd_base_t(const char*      filename,
                 fd_combined_flag flags              = fd_flag::write_only | fd_flag::create | fd_flag::trunc,
                 file_permissions create_permissions = file_permissions::user_rw | file_permissions::group_read |
                                                       file_permissions::other_read):
        fd(Impl::impl_open(filename, flags, create_permissions)) {}

    outfd_base_t(const std::string& filename, file_permissions create_permissions):
        outfd_base_t(filename.data(), create_permissions) {}

    outfd_base_t(const std::string& filename,
                 fd_combined_flag   flags              = fd_flag::write_only | fd_flag::create | fd_flag::trunc,
                 file_permissions   create_permissions = file_permissions::user_rw | file_permissions::group_read |
                                                       file_permissions::other_read):
        outfd_base_t(filename.data(), flags, create_permissions) {}

    outfd_base_t(const outfd_base_t&) = delete;
    outfd_base_t& operator=(const outfd_base_t&) = delete;

    constexpr static bool is_greater_multiple_of_smaller(auto one, auto two) {
        return (one < two || one % two == 0) && (two < one || two % one == 0);
    }

    template <typename TT>
    constexpr static bool size_cast(size_t sz) {
        return sizeof(TT) > sizeof(T) ? sz * (sizeof(TT) / sizeof(T)) : sz / (sizeof(T) / sizeof(TT));
    }

    template <typename DerivedT2, typename Impl2, typename TT, size_t SS, auto... SSettings>
    outfd_base_t(outfd_base_t<DerivedT2, Impl2, TT, SS, SSettings...>&& of) noexcept(
        !exception_on_syswrite_fail ||
        (is_greater_multiple_of_smaller(sizeof(T), sizeof(TT)) && sizeof(SS) <= sizeof(S))):
        fd(of.fd) {

        if constexpr (!is_greater_multiple_of_smaller(sizeof(T), sizeof(TT)))
            of.flush();

        auto can_take_buff = of.size * sizeof(TT) <= S * sizeof(T);
        if (can_take_buff) {
            ::memcpy(buf, of.buf, of.size * sizeof(TT));
            size    = size_cast<TT>(of.size);
            pos     = size_cast<TT>(of.pos);
        }
        else {
            of.flush();
            size = 0;
            pos  = 0;
        }
        of.fd = STDOUT_FILENO;
    }

    template <typename DerivedT2, typename Impl2, typename TT, size_t SS, auto... SSettings>
    outfd_base_t&
    operator=(outfd_base_t<DerivedT2, Impl2, TT, SS, SSettings...>&& of) noexcept(!exception_on_syswrite_fail) {
        if constexpr (std::is_same_v<DerivedT, DerivedT2>) {
            if (&of == this)
                return *this;
        }

        if constexpr (!is_greater_multiple_of_smaller(sizeof(T), sizeof(TT)))
            of.flush();

        if (size != 0)
            Impl::impl_write(fd, buf, size * sizeof(T));

        Impl::impl_close(fd);

        fd = of.fd;

        auto can_take_buff = of.size * sizeof(TT) <= S * sizeof(T);
        if (can_take_buff) {
            ::memcpy(buf, of.buf, of.size * sizeof(TT));
            size    = size_cast<TT>(of.size);
            pos     = size_cast<TT>(of.pos);
            of.size = 0;
            of.pos  = 0;
        }
        else {
            of.flush();
            size = 0;
            pos  = 0;
        }
        of.fd = 0;
        return *this;
    }

    ~outfd_base_t() {
        if (size != 0)
            Impl::impl_write(fd, buf, size * sizeof(T));
        Impl::impl_close(fd);
    }

    DerivedT& write(const T* data, size_t count) noexcept(!exception_on_syswrite_fail) {
        if (count >= S) {
            /* Data do not actualy wraps into buffer */
            flush();
            Impl::impl_write(fd, data, count * sizeof(T));
        }
        else {
            auto free_space = S - pos;
            if (free_space < count)
                flush();

            ::memcpy(buf + pos, data, count * sizeof(T));
            pos += count;

            if (size < pos)
                size = pos;
        }
        return static_cast<DerivedT&>(*this);
    }

    /* May violate strict aliasing rule */
    DerivedT& flatcopy(auto&&... some) {
        return (write(reinterpret_cast<const T*>(&some), sizeof(some)), ...); // NOLINT
    }

    template <size_t N>
    DerivedT& write_any(const char(&c_string)[N]) noexcept(!exception_on_syswrite_fail) {
        return write(c_string, N - 1);
    }

    DerivedT& write_any(std::convertible_to<T> auto&& single_char) noexcept(!exception_on_syswrite_fail) {
        return put(single_char);
    }

    DerivedT& write_any(outfd_concepts::WritableBase<T> auto&& something_flat) noexcept(!exception_on_syswrite_fail) {
        using std::begin, std::end;
        return write(begin(something_flat),
                     static_cast<size_t>(end(something_flat) - begin(something_flat)));
    }

    DerivedT& write_any(outfd_concepts::WritableRAI<T> auto&& rai_container) noexcept(!exception_on_syswrite_fail) {
        using std::begin, std::end;
        return write(begin(rai_container).base(),
                     static_cast<size_t>(end(rai_container) - begin(rai_container)));
    }

    DerivedT& write(outfd_concepts::Writable<T> auto&&... some) noexcept(!exception_on_syswrite_fail) {
        (write_any(some), ...);
        return static_cast<DerivedT&>(*this);
    }

    bool flush() noexcept(!exception_on_syswrite_fail) {
        if (size) {
            Impl::impl_write(fd, buf, sizeof(T) * pos);

            if (pos < size) {
                ::memmove(buf, buf + pos, (size - pos) * sizeof(T));
                size = size - pos;
            }
            else
                size = 0;

            pos = 0;
            return true;
        }
        return false;
    }

    DerivedT& put(T data) noexcept(!exception_on_syswrite_fail) {
        if (size >= S)
            flush();

        buf[pos++] = data;

        if (pos > size)
            size = pos;

        return static_cast<DerivedT&>(*this);
    }

    DerivedT& seek(ssize_t value) noexcept(!exception_on_seek_fail && !exception_on_syswrite_fail) {
        if (value == 0)
            return static_cast<DerivedT&>(*this);

        if ((value < 0 && -value <= pos) || (value > 0 && value <= size - pos)) {
            pos += value;
            return static_cast<DerivedT&>(*this);
        }

        flush();
        Impl::impl_seek(fd, value);
        return static_cast<DerivedT&>(*this);
    }

    bool is_fifo() const {
        if (!stat_executed) {
            is_fifo_fd   = Impl::impl_is_blocking(fd);
            stat_executed = true;
        }
        return is_fifo_fd;
    }

    const fd_type& descriptor() const {
        return fd;
    }

private:
    fd_type fd   = Impl::default_outfd();
    size_t  size = 0;
    size_t  pos  = 0;
    T       buf[S];

    mutable bool is_fifo_fd    = false;
    mutable bool stat_executed = false;
};

#define DECLARE_OUTFD(NAME, IMPL)                                                                                      \
    template <typename T = char, size_t S = 8192, auto... Settings>                                                    \
    struct NAME : public outfd_base_t<NAME<T, S, Settings...>,                                                         \
                                      IMPL<NAME<T, S, Settings...>, Settings...>,                                      \
                                      T,                                                                               \
                                      S,                                                                               \
                                      Settings...> {                                                                   \
        using outfd_base_t<NAME<T, S, Settings...>, IMPL<NAME<T, S, Settings...>, Settings...>, T, S, Settings...>::   \
            outfd_base_t;                                                                                              \
    }

#define DECLARE_INFD(NAME, IMPL)                                                                                       \
    template <typename T = char, size_t S = 8192, auto... Settings>                                                    \
    struct NAME                                                                                                        \
        : public infd_base_t<NAME<T, S, Settings...>, IMPL<NAME<T, S, Settings...>, Settings...>, T, S, Settings...> { \
        using infd_base_t<NAME<T, S, Settings...>, IMPL<NAME<T, S, Settings...>, Settings...>, T, S, Settings...>::    \
            infd_base_t;                                                                                               \
    }

DECLARE_OUTFD(outfd, outfd_linux_impl_t);
DECLARE_OUTFD(outfd_devnull, outfd_devnull_impl);

namespace details {
    struct autocast {
        template <typename T>
        operator T() {
            return {};
        }
    };
}

template <typename T>
concept Outfd = requires (T o) {
    {o.write(details::autocast{}, size_t{})} -> std::same_as<T&>;
    {o.flatcopy(0)} -> std::same_as<T&>;
    {o.flush()} -> std::same_as<bool>;
    {o.seek(size_t{})} -> std::same_as<T&>;
    {o.put(details::autocast{})} -> std::same_as<T&>;
    {o.is_fifo()} -> std::same_as<bool>;
    {o.descriptor()};
};

template <typename T>
concept Infd = requires (T i) {
    {i.read(details::autocast{}, size_t{})} -> std::same_as<io_read_res>;
    {i.nonblock_read(details::autocast{}, size_t{})} -> std::same_as<io_read_res>;
    {i.is_blocking()} -> std::same_as<bool>;
    {i.read_wait_timeout()};
    {i.read_wait_timeout(details::autocast{})};
    {i.can_be_blocked()} -> std::same_as<bool>;
    {i.descriptor()};
};

template <typename DerivedT, typename Impl, typename T = char, size_t S = 8192, auto... Settings>
class infd_base_t : public Impl { // NOLINT
public:
    template <typename DerivedT2, typename Impl2, typename TT, size_t SS, auto... SSettings>
    friend class infd_base_t;

    using fd_type = decltype(Impl::impl_open("", fd_flag{}, file_permissions{}));

    static constexpr bool exception_on_seek_fail = details::check_enable<fd_exception_on_seek_fail>(Settings...);
    static constexpr bool exception_on_read_fail = details::check_enable<fd_exception_on_read_fail>(Settings...);

    constexpr static DerivedT raw_create(fd_type fd) {
        DerivedT infd;
        infd.fd = fd;
        return infd;
    }

    infd_base_t() = default;

    infd_base_t(const char* filename, fd_combined_flag flags = fd_flag::read_only):
        fd(Impl::impl_open(filename, flags, file_permissions{})) {}

    infd_base_t(const std::string& filename, fd_combined_flag flags = fd_flag::read_only):
        infd_base_t(filename.data(), flags) {}

    infd_base_t(infd_base_t&&) noexcept = default;
    infd_base_t& operator=(infd_base_t&&) noexcept = default;

    ~infd_base_t() {
        Impl::impl_close(fd);
    }

    io_read_res read(T* destination, size_t count) noexcept(!exception_on_read_fail) {
        if (count > S) {
            auto wrote_size                = flush_to_dst(destination);
            auto reminder_size             = count - wrote_size;
            auto [wrote_reminder_size, rc] = Impl::impl_read(fd, destination + wrote_size, reminder_size * sizeof(T));
            auto actually_read             = wrote_size + wrote_reminder_size;
            return {actually_read, rc};
        }

        auto rc = io_read_rc::ok;
        if (size == 0)
            rc = take_next();

        if (rc != io_read_rc::ok && rc != io_read_rc::partial)
            return {0, rc};

        if (count <= size) {
            ::memcpy(destination, buf + pos, count * sizeof(T));
            size -= count;
            pos  += count;
            return {count, io_read_rc::ok};
        }
        else {
            auto actually_read = flush_to_dst(destination);
            return {actually_read, io_read_rc::partial};
        }
    }

    io_read_res nonblock_read(T* destination, size_t count) {
        if (blocking && can_be_blocked()) {
            if (Impl::impl_waitdev(fd, wait_timeout.count()))
                return read(destination, count);
            else
                return {0, io_read_rc::timeout};
        }
        else
            return read(destination, count);
    }

    bool is_blocking() const noexcept {
        return blocking;
    }

    auto read_wait_timeout() const noexcept {
        return wait_timeout;
    }

    void read_wait_timeout(auto duration) noexcept {
        wait_timeout = duration;
    }

    bool can_be_blocked() const {
        if (!stat_executed) {
            can_blocked   = Impl::impl_is_blocking(fd);
            stat_executed = true;
        }
        return can_blocked;
    }

    const fd_type& descriptor() const {
        return fd;
    }

private:
    io_read_rc take_next() {
        auto [actually_read, rc] = Impl::impl_read(fd, buf, S * sizeof(T));
        size                     = actually_read;
        pos                      = 0;
        return rc;
    }

    size_t flush_to_dst(T* dst) noexcept {
        if (size != 0) {
            size_t actually_read = size;
            ::memcpy(dst, buf + pos, size * sizeof(T));
            size = 0;
            pos  = 0;
            return actually_read;
        }
        return 0;
    }

private:
    fd_type                   fd   = Impl::default_infd();
    size_t                    size = 0;
    size_t                    pos  = 0;
    T                         buf[S];
    std::chrono::microseconds wait_timeout{0};
    bool                      blocking      = true;
    mutable bool              can_blocked   = false;
    mutable bool              stat_executed = false;
};

DECLARE_INFD(infd, outfd_linux_impl_t);

template <typename T, bool Mutable = false, bool autoclose_fd_on_fail = true>
class mmap_file_range {
public:
    /* Leaky abstraction :( */
    template <typename>
    friend class file_view;

    using value_type = std::conditional_t<Mutable, T, const T>;

    mmap_file_range() = default;

#ifdef _WIN32

    mmap_file_range(const char* filename) {
        int fd = impl_open_posix(filename, Mutable ? O_RDWR : O_RDONLY, 0);
        if (fd < 0)
            throw cannot_open_file(filename, errc::from_errno());

        auto guard = exception_guard([&]{
            if constexpr (autoclose_fd_on_fail)
                impl_close_posix(fd);
        });

        struct stat st = {};
        if (impl_fstat_posix(fd, &st) < 0)
            throw stat_fd_failed(filename, errc::from_errno(), autoclose_fd_on_fail ? -1 : fd);

        auto total = static_cast<size_t>(st.st_size) / sizeof(T);

        /*
         * No file mappings here: on some Windows volumes (VMs, sync folders) the size a
         * mapping is created with can disagree with fstat, so a view can be shorter than
         * the reported size and reading past it faults. Plain sequential reads are always
         * coherent; keep the same interface on a heap buffer instead.
         */
        auto* buf = new T[total](); // NOLINT (total == 0 gives a valid empty buffer)
        auto* dst  = buf;
        auto  left = total;
        while (left != 0) {
            auto avail = impl_read_posix(fd, dst, left * sizeof(T));
            if (avail < 0) {
                delete[] buf; // NOLINT
                throw mmap_fd_failed(filename, errc::from_errno(), autoclose_fd_on_fail ? -1 : fd);
            }
            if (avail == 0)
                break; // stale size metadata (some Windows volumes); use what was actually read
            dst   += avail;
            left -= static_cast<size_t>(avail);
        }

        impl_close_posix(fd);
        guard.dismiss();

        start = buf;
        pend  = start + (total - left);
    }

    ~mmap_file_range() {
        delete[] start; // NOLINT
    }

    mmap_file_range(mmap_file_range&& mfr) noexcept: start(mfr.start), pend(mfr.pend) {
        mfr.start = nullptr;
        mfr.pend  = nullptr;
    }

    mmap_file_range& operator=(mmap_file_range&& mfr) noexcept {
        if (&mfr == this)
            return *this;

        delete[] start; // NOLINT

        start     = mfr.start;
        pend      = mfr.pend;
        mfr.start = nullptr;
        mfr.pend  = nullptr;
        return *this;
    }

#else

    mmap_file_range(const char* filename) {
        int fd = open(filename, Mutable ? O_RDWR : O_RDONLY);
        if (fd < 0)
            throw cannot_open_file(filename, errc::from_errno());

        auto guard = exception_guard([&]{
            if constexpr (autoclose_fd_on_fail)
                ::close(fd);
        });

        struct stat st;
        if (fstat(fd, &st) < 0)
            throw stat_fd_failed(filename, errc::from_errno(), autoclose_fd_on_fail ? -1 : fd);

        auto size = static_cast<size_t>(st.st_size) / sizeof(T);
        if (size != 0) {
            if ((start = static_cast<T*>(mmap(nullptr,
                                              size_t(st.st_size),
                                              PROT_READ | (Mutable ? PROT_WRITE : 0),
                                              Mutable ? MAP_SHARED : MAP_PRIVATE,
                                              fd,
                                              0))) == MAP_FAILED) // NOLINT
                throw mmap_fd_failed(filename, errc::from_errno(), autoclose_fd_on_fail ? -1 : fd);

            pend = start + size;
        }

        /* Explicitly close on successfull exit */
        ::close(fd);
    }

    ~mmap_file_range() {
        if (start)
            ::munmap(start, static_cast<size_t>(pend - start));
    }

    mmap_file_range(mmap_file_range&& mfr) noexcept: start(mfr.start), pend(mfr.pend) {
        mfr.start = nullptr;
    }

    mmap_file_range& operator=(mmap_file_range&& mfr) noexcept {
        if (&mfr == this)
            return *this;

        if (start)
            ::munmap(start, static_cast<size_t>(pend - start));

        start     = mfr.start;
        pend      = mfr.pend;
        mfr.start = nullptr;
        return *this;
    }

#endif

    const value_type* begin() const {
        return start;
    }

    const value_type* end() const {
        return pend;
    }

    value_type* begin() {
        return start;
    }

    value_type* end() {
        return pend;
    }

    const value_type* data() const {
        return begin();
    }

    value_type* data() {
        return begin();
    }

    [[nodiscard]]
    size_t size() const {
        return static_cast<size_t>(pend - start);
    }

private:
    T* start = nullptr;
    T* pend  = nullptr;
};

template <typename T>
class file_view {
public:
    file_view() = default;

    file_view(const char* filename) {
        mfr = decltype(mfr)(filename);
        int fd;
        try {
            mfr = mmap_file_range<T, false, false>(filename);
            return;
        }
        catch (const stat_fd_failed& e) {
            fd = e.unclosed_fd;
        }
        catch (const mmap_fd_failed& e) {
            fd = e.unclosed_fd;
        }

        really_mmaped = false;
        read_buffered(fd);
    }

    file_view(file_view&& fv) noexcept = default;

    file_view& operator=(file_view&& fv) noexcept {
        if (&fv == this)
            return *this;

        if (!really_mmaped) {
            delete[] mfr.start;
            mfr.start = nullptr;
            mfr.pend  = nullptr;
        }
        mfr = std::move(fv.mfr);
        really_mmaped = fv.really_mmaped;

        return *this;
    }

    ~file_view() {
        if (!really_mmaped) {
            delete [] mfr.start;
            mfr.start = nullptr;
            mfr.pend = nullptr;
        }
    }

    const T* begin() const {
        return mfr.begin();
    }

    const T* end() const {
        return mfr.end();
    }

    bool is_mmaped() const {
        return really_mmaped;
    }

    auto& mmap_range() {
        return mfr;
    }

    auto& mmap_range() const {
        return mfr;
    }

    const T* data() const {
        return begin();
    }

    [[nodiscard]]
    size_t size() const {
        return static_cast<size_t>(end() - begin());
    }

private:
    void read_buffered(int fd) {
        auto scope_exit = finalizer([=] { impl_close_posix(fd); });

        static constexpr size_t buff_size = 8192 / sizeof(T);
        T                       buff[buff_size];
        ssize_t                 avail = 0;

        size_t allocated = 0;
        size_t size      = 0;
        auto&  data      = mfr.start;

        while ((avail = impl_read_posix(fd, buff, buff_size * sizeof(T))) > 0) {
            auto required_allocated = size + size_t(avail);
            if (allocated < required_allocated) {
                auto new_allocated = allocated * 2;
                if (new_allocated < required_allocated)
                    new_allocated = required_allocated;

                auto new_data = new T[new_allocated]; // NOLINT
                ::memcpy(new_data, data, size * sizeof(T));
                delete[] data; // NOLINT
                data      = new_data;
                allocated = new_allocated;
            }

            ::memcpy(data + size, buff, size_t(avail) * sizeof(T));
            size += size_t(avail);
        }

        if (avail < 0) {
            delete[] data; // NOLINT
            throw sys_read_fail(errc::from_errno());
        }

        mfr.pend = mfr.start + size;
    }

private:
    mmap_file_range<T, false, false> mfr;
    bool                             really_mmaped = true;
};

} // namespace dfdh
