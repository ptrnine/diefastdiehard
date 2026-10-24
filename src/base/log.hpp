#pragma once

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <map>
#include <atomic>

#include "io.hpp"
#include "time.hpp"
#include "ring_buffer.hpp"
#include "print.hpp"
#include "hash_functions.hpp"
#include "split_view.hpp"

namespace dfdh
{
inline constexpr auto log_time_format = "[hh:mm:ss.xxx]"sv;
enum class log_level { debug = 0, detail, info, warn, error };

class log_acceptor_base {
public:
    enum class write_type {
        new_record = 0,
        write_same,
        update
    };

    virtual ~log_acceptor_base() = default;

    virtual void
    write_handler(log_level level, write_type wt, std::string_view time, std::string_view msg, uint64_t times) = 0;

    void write(log_level level, std::string_view time, std::string_view msg, uint64_t msg_hash) {
        auto     comphash   = uint32_t((msg_hash & 0x00000000ffffffff) ^ (msg_hash >> 32));
        uint64_t d          = data.load(std::memory_order_acquire);
        auto     prev_hash  = unpack_hash(d);
        auto     same_times = unpack_times(d);

        auto wt = write_type::new_record;

        if (comphash != prev_hash)
            same_times = 0;
        else
            wt = write_type::write_same;
        if (same_times != std::numeric_limits<uint16_t>::max())
            ++same_times;

        data.store(pack_data(comphash, same_times, 0), std::memory_order_release);
        write_handler(level, wt, time, msg, same_times);
    }

    void write_update(uint16_t update_id, log_level level, std::string_view time, std::string_view msg) {
        uint64_t d              = data.load(std::memory_order_acquire);
        auto     hash           = unpack_hash(d);
        auto     same_times     = unpack_times(d);
        auto     prev_update_id = unpack_update_id(d);

        auto wt = write_type::update;
        if (update_id != prev_update_id) {
            same_times = 0;
            wt = write_type::new_record;
        }
        if (same_times != std::numeric_limits<uint16_t>::max())
            ++same_times;

        data.store(pack_data(hash, same_times, update_id), std::memory_order_release);
        write_handler(level, wt, time, msg, same_times);
    }

private:
    static uint32_t unpack_hash(uint64_t v) {
        return uint32_t(v >> 32);
    }

    static uint16_t unpack_times(uint64_t v) {
        return uint16_t((v & 0x00000000ffff0000) >> 16);
    }

    static uint16_t unpack_update_id(uint64_t v) {
        return uint16_t(v & 0x000000000000ffff);
    }

    static uint64_t pack_data(uint32_t comphash, uint16_t times, uint16_t update_id) {
        return (uint64_t(comphash) << 32) | (uint64_t(times) << 16) | uint64_t(update_id);
    }

private:
    std::atomic<uint64_t> data = 0;
};

class log_acceptor_fd : public log_acceptor_base {
public:
    log_acceptor_fd(outfd<char> o): ofd(std::move(o)), is_fifo(ofd.is_fifo()), is_tty(isatty(ofd.descriptor())) {}

    static std::unique_ptr<log_acceptor_fd> create(outfd<char> ofd) {
        return std::make_unique<log_acceptor_fd>(std::move(ofd));
    }

    void
    write_handler(log_level level, write_type wt, std::string_view time, std::string_view msg, uint64_t times) final {
        static constexpr std::string_view level_str[] = {
            ": [debug] "sv,
            ": "sv,
            ": [info] "sv,
            ": [warn] "sv,
            ": [error] "sv
        };
        static constexpr std::string_view level_color[] = {
            "\033[0;38;5;12m",
            "\033[0;38;5;7m",
            "\033[0;38;5;10m",
            "\033[0;38;5;11m",
            "\033[0;38;5;1m",
        };

        std::string record;

        if (wt != write_type::new_record && is_fifo)
            record += '\r';
        else
            record += '\n';

        /* Setup color for log level */
        if (is_tty)
            record += level_color[size_t(level)];

        record += time;

        /* Enable bold */
        if (is_tty)
            record += "\033[1m";

        if (times > 1 && wt == write_type::write_same) {
            if (times != std::numeric_limits<uint16_t>::max()) {
                record += " (";
                record += std::to_string(times);
                record += " times)";
            }
            else {
                record += " (repeats infinitely)";
            }
        }
        record += level_str[size_t(level)];

        /* Disable bold */
        if (is_tty)
            record += "\033[22m";

        record += msg;

        /* Reset colors */
        if (is_tty)
            record += "\033[0m";

        /* XXX: racy */
        size_t prev_len = prev_record_len.exchange(record.size());
        if (wt != write_type::new_record && !is_fifo)
            ofd.impl_seek(ofd.descriptor(), -ssize_t(prev_len));
        ofd.impl_write(ofd.descriptor(), record.data(), record.size());
    }

private:
    outfd<char>         ofd;
    const bool          is_fifo;
    const bool          is_tty;
    std::atomic<size_t> prev_record_len = 0;
};

template <typename T>
class log_records {
public:
    template <typename I>
    log_records(I ibegin, I iend, size_t start_pos, size_t end_pos, size_t max_pos):
        mstart_pos(start_pos), mend_pos(end_pos), mmax_pos(max_pos) {
        records.resize(end_pos - start_pos);
        size_t i = 0;
        for (auto iter = ibegin; iter != iend; ++iter) records[i++] = *iter;
    }

    [[nodiscard]]
    auto begin() const {
        return records.begin();
    }

    [[nodiscard]]
    auto end() const {
        return records.end();
    }

    [[nodiscard]]
    size_t start_pos() const {
        return mstart_pos;
    }

    [[nodiscard]]
    size_t end_pos() const {
        return mend_pos;
    }

    [[nodiscard]]
    size_t max_pos() const {
        return mmax_pos;
    }

private:
    std::vector<T> records;
    size_t mstart_pos;
    size_t mend_pos;
    size_t mmax_pos;
};

template <typename I>
log_records(I i, I, size_t, size_t, size_t) -> log_records<std::decay_t<decltype(*i)>>;

template <typename I>
class locked_log_range {
public:
    locked_log_range(I ibegin, I iend, size_t start_pos, size_t end_pos, size_t max_pos, std::shared_mutex& imtx):
        b(ibegin), e(iend), mstart_pos(start_pos), mend_pos(end_pos), mmax_pos(max_pos), mtx(imtx) {}

    ~locked_log_range() {
        mtx.unlock_shared();
    }

    [[nodiscard]]
    I begin() const {
        return b;
    }

    [[nodiscard]]
    I end() const {
        return e;
    }

    [[nodiscard]]
    size_t start_pos() const {
        return mstart_pos;
    }

    [[nodiscard]]
    size_t end_pos() const {
        return mend_pos;
    }

    [[nodiscard]]
    size_t max_pos() const {
        return mmax_pos;
    }

    [[nodiscard]]
    auto copy_records() const {
        return log_records(b, e, mstart_pos, mend_pos, mmax_pos);
    }

private:
    I                  b, e;
    size_t             mstart_pos;
    size_t             mend_pos;
    size_t             mmax_pos;
    std::shared_mutex& mtx;
};

class log_acceptor_ring_buffer : public log_acceptor_base {
public:
    struct record {
        std::string time;
        std::string msg;
        uint64_t    times;
        log_level   lvl;
        write_type  wt;
    };

    log_acceptor_ring_buffer(size_t max_records): records(max_records) {}
    static std::unique_ptr<log_acceptor_ring_buffer> create(size_t max_records) {
        return std::make_unique<log_acceptor_ring_buffer>(max_records);
    }

    void
    write_handler(log_level level, write_type wt, std::string_view time, std::string_view msg, uint64_t times) final {
        std::unique_lock lock{mtx};

        if (wt == write_type::new_record || records.empty()) {
            std::string tm{time};
            last_lines_count = 0;
            for (auto line : msg / split('\n', '\r')) {
                /* Specify time in first line only */
                records.push({std::move(tm), {line.begin(), line.end()}, times, level, wt});
                ++last_lines_count;
            }
        }
        else if (last_lines_count <= records.size()) {
            auto offset = records.size() - last_lines_count;
            auto b      = records.begin() + ssize_t(offset);

            b->lvl   = level;
            b->time  = time;
            b->wt    = wt;
            b->times = times;

            if (wt == write_type::update) {
                size_t lines_count = 0;
                for (auto line : msg / split('\n', '\r')) {
                    if (b != records.end())
                        b->msg = std::string(line.begin(), line.end());
                    else
                        records.push({{}, {line.begin(), line.end()}, times, level, wt});
                    ++lines_count;
                    ++b;
                }

                while (lines_count < last_lines_count) {
                    records.pop();
                    --lines_count;
                }

                last_lines_count = lines_count;
            }
        }
    }

    auto read_lock(size_t preferred_start, size_t max_records) const {
        mtx.lock_shared();
        if (records.size() < max_records) {
            return locked_log_range(records.begin(), records.end(), 0, records.size(), records.size(), mtx);
        }
        else if (preferred_start + max_records > records.size()) {
            preferred_start = records.size() - max_records;
            return locked_log_range(records.begin() + ssize_t(preferred_start),
                                    records.end(),
                                    preferred_start,
                                    records.size(),
                                    records.size(),
                                    mtx);
        }
        else {
            return locked_log_range(records.begin() + ssize_t(preferred_start),
                                    records.begin() + ssize_t(preferred_start + max_records),
                                    preferred_start,
                                    preferred_start + max_records,
                                    records.size(),
                                    mtx);
        }
    }

    auto get_records(size_t preferred_start, size_t max_records) const {
        return read_lock(preferred_start, max_records).copy_records();
    }

    [[nodiscard]]
    size_t size() const {
        std::shared_lock lock{mtx};
        return records.size();
    }

    [[nodiscard]]
    size_t max_size() const {
        std::shared_lock lock{mtx};
        return records.max_size();
    }

    void max_size(size_t value) {
        std::unique_lock lock{mtx};
        records.resize(value);
    }

    void clear() {
        std::unique_lock lock{mtx};
        records.clear();
    }

private:
    ring_buffer<record>       records;
    size_t                    last_lines_count = 0;
    mutable std::shared_mutex mtx;
};

class logger {
public:
    logger() {
        add_stream("stdout", log_acceptor_fd::create(outfd<char>::stdout()));
    }

    ~logger() {
        info("******* log close *******\n");
    }

    void add_stream(const std::string& name, std::unique_ptr<log_acceptor_base> log_acceptor) {
        std::unique_lock lock{mtx};
        streams.insert_or_assign(name, std::move(log_acceptor));
    }

    void add_stream(const std::string& name, outfd<char> ofd) {
        std::unique_lock lock{mtx};
        streams.insert_or_assign(name, log_acceptor_fd::create(std::move(ofd)));
    }

    void remove_stream(const std::string& name) {
        std::unique_lock lock{mtx};
        streams.erase(name);
    }

    std::unique_ptr<log_acceptor_base> take_stream(const std::string& name) {
        std::unique_lock lock{mtx};
        auto found = streams.find(name);
        if (found != streams.end()) {
            auto res = std::move(found->second);
            streams.erase(found);
            return res;
        }
        return {};
    }

    template <typename... Ts>
    void log(log_level level, std::string_view format_str, Ts&&... args) {
        auto msg  = dfdh::format(format_str, std::forward<Ts>(args)...);
        auto hash = fnv1a64(msg.data(), msg.size());
        auto time = current_datetime(log_time_format);

        std::shared_lock lock{mtx};
        for (auto& [_, stream] : streams) stream->write(level, time, msg, hash);
    }

    template <typename... Ts>
    void log_update(log_level level, uint16_t update_id, std::string_view format_str, Ts&&... args) {
        auto msg  = dfdh::format(format_str, std::forward<Ts>(args)...);
        auto time = current_datetime(log_time_format);

        std::shared_lock lock{mtx};
        for (auto& [_, stream] : streams) stream->write_update(update_id, level, time, msg);
    }

#define def_log_func(level)                                                                                            \
    template <typename... Ts>                                                                                          \
    void level(std::string_view format_str, Ts&&... args) {                                                            \
        log(log_level::level, format_str, std::forward<Ts>(args)...);                                                  \
    }                                                                                                                  \
    template <typename... Ts>                                                                                          \
    void level##_update(uint16_t update_id, std::string_view format_str, Ts&&... args) {                               \
        log_update(log_level::level, update_id, format_str, std::forward<Ts>(args)...);                                \
    }

    def_log_func(debug)
    def_log_func(detail)
    def_log_func(info)
    def_log_func(warn)
    def_log_func(error)
#undef def_log_func

private:
    std::map<std::string, std::unique_ptr<log_acceptor_base>> streams;
    mutable std::shared_mutex                                 mtx;
};

/* Global logger */
static logger& glog() {
    static logger logr;
    return logr;
}

} // namespace dfdh
