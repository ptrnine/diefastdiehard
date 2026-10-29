#pragma once

#include <optional>
#include <string>
#include <utility>

#ifndef _WIN32
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "base/log.hpp"

namespace dfdh {

/* A named pipe for lua code (--lua-exec-fifo): everything written between the open and the close of
 * the writers is one chunk (`echo 'code' > fifo`, a heredoc). Polled from the main loop, never blocks.
 * The fifo is recreated on startup and kept on exit: a writer to a dead game blocks instead of
 * silently creating a regular file in its place */
class lua_fifo {
public:
    lua_fifo(std::string path): _path(std::move(path)) {
#ifdef _WIN32
        glog().error("--lua-exec-fifo: named pipes are not supported on Windows");
#else
        struct stat st;
        if (::lstat(_path.data(), &st) == 0) {
            /* Do not delete what is not ours: a mistyped path may point to a real file */
            if (!S_ISFIFO(st.st_mode)) {
                glog().error("--lua-exec-fifo: {} exists and is not a fifo", _path);
                return;
            }
            ::unlink(_path.data());
        }

        if (::mkfifo(_path.data(), 0600) != 0) {
            glog().error("--lua-exec-fifo: cannot create {}: {}", _path, std::strerror(errno));
            return;
        }

        /* Nonblocking: open does not wait for a writer, read returns EAGAIN while a writer is connected
         * and 0 (EOF) when there are none */
        _fd = ::open(_path.data(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (_fd < 0)
            glog().error("--lua-exec-fifo: cannot open {}: {}", _path, std::strerror(errno));
        else
            glog().info("lua fifo: listening on {}", _path);
#endif
    }

    ~lua_fifo() {
#ifndef _WIN32
        if (_fd >= 0)
            ::close(_fd);
#endif
    }

    lua_fifo(const lua_fifo&)            = delete;
    lua_fifo& operator=(const lua_fifo&) = delete;

    /* Returns a chunk once all its writers have closed the fifo */
    std::optional<std::string> poll() {
#ifndef _WIN32
        if (_fd < 0)
            return {};

        char buf[4096];
        while (true) {
            auto n = ::read(_fd, buf, sizeof(buf));
            if (n > 0) {
                _chunk.append(buf, size_t(n));
                continue;
            }

            if (n == 0) {
                if (_chunk.empty())
                    return {};
                return std::exchange(_chunk, {});
            }

            if (errno == EAGAIN || errno == EINTR)
                return {};

            glog().error("lua fifo: read from {} failed: {}", _path, std::strerror(errno));
            ::close(_fd);
            _fd = -1;
            return {};
        }
#else
        return {};
#endif
    }

private:
    std::string _path;
    std::string _chunk;
    int         _fd = -1;
};

} // namespace dfdh
