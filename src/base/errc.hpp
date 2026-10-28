#pragma once

#include <string>
#include <cerrno>
#include <ostream>

#ifdef _WIN32

/* Exotic POSIX errno codes that MinGW's <errno.h> doesn't define. They never
 * occur on Windows; the values exist only so the errno->string map below
 * compiles uniformly. */
#define EADV 10002
#define EBADE 10003
#define EBADFD 10004
#define EBADR 10005
#define EBADRQC 10006
#define EBADSLT 10007
#define EBFONT 10008
#define ECHRNG 10009
#define ECOMM 10010
#define EDOTDOT 10011
#define EDQUOT 10012
#define EHOSTDOWN 10013
#define EISNAM 10014
#define EKEYEXPIRED 10015
#define EKEYREJECTED 10016
#define EKEYREVOKED 10017
#define EL2HLT 10018
#define EL2NSYNC 10019
#define EL3HLT 10020
#define EL3RST 10021
#define ELIBACC 10022
#define ELIBBAD 10023
#define ELIBEXEC 10024
#define ELIBMAX 10025
#define ELIBSCN 10026
#define ELNRNG 10027
#define EMEDIUMTYPE 10028
#define EMULTIHOP 10029
#define ENAVAIL 10030
#define ENOANO 10031
#define ENOCSI 10032
#define ENOKEY 10033
#define ENOMEDIUM 10034
#define ENONET 10035
#define ENOPKG 10036
#define ENOTBLK 10037
#define ENOTNAM 10038
#define ENOTUNIQ 10039
#define EPFNOSUPPORT 10040
#define EREMCHG 10041
#define EREMOTE 10042
#define EREMOTEIO 10043
#define ERESTART 10044
#define ESHUTDOWN 10045
#define ESOCKTNOSUPPORT 10046
#define ESRMNT 10047
#define ESTALE 10048
#define ESTRPIPE 10049
#define ETOOMANYREFS 10050
#define EUCLEAN 10051
#define EUNATCH 10052
#define EUSERS 10053
#define EXFULL 10054

#endif

namespace dfdh
{
using namespace std::string_view_literals;

struct errc {
    static errc from_errno() {
        return {errno};
    }

    [[nodiscard]]
    std::string_view to_string() const {
        switch (code) {
        case 0: return "OK"sv;
        case EPERM: return "EPERM"sv;
        case ENOENT: return "ENOENT"sv;
        case ESRCH: return "ESRCH"sv;
        case EINTR: return "EINTR"sv;
        case EIO: return "EIO"sv;
        case ENXIO: return "ENXIO"sv;
        case E2BIG: return "E2BIG"sv;
        case ENOEXEC: return "ENOEXEC"sv;
        case EBADF: return "EBADF"sv;
        case ECHILD: return "ECHILD"sv;
        case EAGAIN: return "EAGAIN"sv;
        case ENOMEM: return "ENOMEM"sv;
        case EACCES: return "EACCES"sv;
        case EFAULT: return "EFAULT"sv;
        case ENOTBLK: return "ENOTBLK"sv;
        case EBUSY: return "EBUSY"sv;
        case EEXIST: return "EEXIST"sv;
        case EXDEV: return "EXDEV"sv;
        case ENODEV: return "ENODEV"sv;
        case ENOTDIR: return "ENOTDIR"sv;
        case EISDIR: return "EISDIR"sv;
        case EINVAL: return "EINVAL"sv;
        case ENFILE: return "ENFILE"sv;
        case EMFILE: return "EMFILE"sv;
        case ENOTTY: return "ENOTTY"sv;
        case ETXTBSY: return "ETXTBSY"sv;
        case EFBIG: return "EFBIG"sv;
        case ENOSPC: return "ENOSPC"sv;
        case ESPIPE: return "ESPIPE"sv;
        case EROFS: return "EROFS"sv;
        case EMLINK: return "EMLINK"sv;
        case EPIPE: return "EPIPE"sv;
        case EDOM: return "EDOM"sv;
        case ERANGE: return "ERANGE"sv;
        case EDEADLK: return "EDEADLK"sv;
        case ENAMETOOLONG: return "ENAMETOOLONG"sv;
        case ENOLCK: return "ENOLCK"sv;
        case ENOSYS: return "ENOSYS"sv;
        case ENOTEMPTY: return "ENOTEMPTY"sv;
        case ELOOP: return "ELOOP"sv;
        case ENOMSG: return "ENOMSG"sv;
        case EIDRM: return "EIDRM"sv;
        case ECHRNG: return "ECHRNG"sv;
        case EL2NSYNC: return "EL2NSYNC"sv;
        case EL3HLT: return "EL3HLT"sv;
        case EL3RST: return "EL3RST"sv;
        case ELNRNG: return "ELNRNG"sv;
        case EUNATCH: return "EUNATCH"sv;
        case ENOCSI: return "ENOCSI"sv;
        case EL2HLT: return "EL2HLT"sv;
        case EBADE: return "EBADE"sv;
        case EBADR: return "EBADR"sv;
        case EXFULL: return "EXFULL"sv;
        case ENOANO: return "ENOANO"sv;
        case EBADRQC: return "EBADRQC"sv;
        case EBADSLT: return "EBADSLT"sv;
        case EBFONT: return "EBFONT"sv;
        case ENOSTR: return "ENOSTR"sv;
        case ENODATA: return "ENODATA"sv;
        case ETIME: return "ETIME"sv;
        case ENOSR: return "ENOSR"sv;
        case ENONET: return "ENONET"sv;
        case ENOPKG: return "ENOPKG"sv;
        case EREMOTE: return "EREMOTE"sv;
        case ENOLINK: return "ENOLINK"sv;
        case EADV: return "EADV"sv;
        case ESRMNT: return "ESRMNT"sv;
        case ECOMM: return "ECOMM"sv;
        case EPROTO: return "EPROTO"sv;
        case EMULTIHOP: return "EMULTIHOP"sv;
        case EDOTDOT: return "EDOTDOT"sv;
        case EBADMSG: return "EBADMSG"sv;
        case EOVERFLOW: return "EOVERFLOW"sv;
        case ENOTUNIQ: return "ENOTUNIQ"sv;
        case EBADFD: return "EBADFD"sv;
        case EREMCHG: return "EREMCHG"sv;
        case ELIBACC: return "ELIBACC"sv;
        case ELIBBAD: return "ELIBBAD"sv;
        case ELIBSCN: return "ELIBSCN"sv;
        case ELIBMAX: return "ELIBMAX"sv;
        case ELIBEXEC: return "ELIBEXEC"sv;
        case EILSEQ: return "EILSEQ"sv;
        case ERESTART: return "ERESTART"sv;
        case ESTRPIPE: return "ESTRPIPE"sv;
        case EUSERS: return "EUSERS"sv;
        case ENOTSOCK: return "ENOTSOCK"sv;
        case EDESTADDRREQ: return "EDESTADDRREQ"sv;
        case EMSGSIZE: return "EMSGSIZE"sv;
        case EPROTOTYPE: return "EPROTOTYPE"sv;
        case ENOPROTOOPT: return "ENOPROTOOPT"sv;
        case EPROTONOSUPPORT: return "EPROTONOSUPPORT"sv;
        case ESOCKTNOSUPPORT: return "ESOCKTNOSUPPORT"sv;
        case EOPNOTSUPP: return "EOPNOTSUPP"sv;
        case EPFNOSUPPORT: return "EPFNOSUPPORT"sv;
        case EAFNOSUPPORT: return "EAFNOSUPPORT"sv;
        case EADDRINUSE: return "EADDRINUSE"sv;
        case EADDRNOTAVAIL: return "EADDRNOTAVAIL"sv;
        case ENETDOWN: return "ENETDOWN"sv;
        case ENETUNREACH: return "ENETUNREACH"sv;
        case ENETRESET: return "ENETRESET"sv;
        case ECONNABORTED: return "ECONNABORTED"sv;
        case ECONNRESET: return "ECONNRESET"sv;
        case ENOBUFS: return "ENOBUFS"sv;
        case EISCONN: return "EISCONN"sv;
        case ENOTCONN: return "ENOTCONN"sv;
        case ESHUTDOWN: return "ESHUTDOWN"sv;
        case ETOOMANYREFS: return "ETOOMANYREFS"sv;
        case ETIMEDOUT: return "ETIMEDOUT"sv;
        case ECONNREFUSED: return "ECONNREFUSED"sv;
        case EHOSTDOWN: return "EHOSTDOWN"sv;
        case EHOSTUNREACH: return "EHOSTUNREACH"sv;
        case EALREADY: return "EALREADY"sv;
        case EINPROGRESS: return "EINPROGRESS"sv;
        case ESTALE: return "ESTALE"sv;
        case EUCLEAN: return "EUCLEAN"sv;
        case ENOTNAM: return "ENOTNAM"sv;
        case ENAVAIL: return "ENAVAIL"sv;
        case EISNAM: return "EISNAM"sv;
        case EREMOTEIO: return "EREMOTEIO"sv;
        case EDQUOT: return "EDQUOT"sv;
        case ENOMEDIUM: return "ENOMEDIUM"sv;
        case EMEDIUMTYPE: return "EMEDIUMTYPE"sv;
        case ECANCELED: return "ECANCELED"sv;
        case ENOKEY: return "ENOKEY"sv;
        case EKEYEXPIRED: return "EKEYEXPIRED"sv;
        case EKEYREVOKED: return "EKEYREVOKED"sv;
        case EKEYREJECTED: return "EKEYREJECTED"sv;
        case EOWNERDEAD: return "EOWNERDEAD"sv;
        case ENOTRECOVERABLE: return "ENOTRECOVERABLE"sv;
        default: return "UNKNOWERROR"sv;
        }
    }

    [[nodiscard]]
    std::string_view description() const {
        switch (code) {
        case 0: return "no error"sv;
        case EPERM: return "operation not permitted"sv;
        case ENOENT: return "no such file or directory"sv;
        case ESRCH: return "no such process"sv;
        case EINTR: return "interrupted system call"sv;
        case EIO: return "i/o error"sv;
        case ENXIO: return "no such device or address"sv;
        case E2BIG: return "argument list too long"sv;
        case ENOEXEC: return "exec format error"sv;
        case EBADF: return "bad file number"sv;
        case ECHILD: return "no child processes"sv;
        case EAGAIN: return "try again"sv;
        case ENOMEM: return "out of memory"sv;
        case EACCES: return "permission denied"sv;
        case EFAULT: return "bad address"sv;
        case ENOTBLK: return "block device required"sv;
        case EBUSY: return "device or resource busy"sv;
        case EEXIST: return "file exists"sv;
        case EXDEV: return "cross-device link"sv;
        case ENODEV: return "no such device"sv;
        case ENOTDIR: return "not a directory"sv;
        case EISDIR: return "is a directory"sv;
        case EINVAL: return "invalid argument"sv;
        case ENFILE: return "file table overflow"sv;
        case EMFILE: return "too many open files"sv;
        case ENOTTY: return "not a typewriter"sv;
        case ETXTBSY: return "text file busy"sv;
        case EFBIG: return "file too large"sv;
        case ENOSPC: return "no space left on device"sv;
        case ESPIPE: return "illegal seek"sv;
        case EROFS: return "read-only file system"sv;
        case EMLINK: return "too many links"sv;
        case EPIPE: return "broken pipe"sv;
        case EDOM: return "math argument out of domain of func"sv;
        case ERANGE: return "math result not representable"sv;
        case EDEADLK: return "resource deadlock would occur"sv;
        case ENAMETOOLONG: return "file name too long"sv;
        case ENOLCK: return "no record locks available"sv;
        case ENOSYS: return "function not implemented"sv;
        case ENOTEMPTY: return "directory not empty"sv;
        case ELOOP: return "too many symbolic links encountered"sv;
        case ENOMSG: return "no message of desired type"sv;
        case EIDRM: return "identifier removed"sv;
        case ECHRNG: return "channel number out of range"sv;
        case EL2NSYNC: return "level 2 not synchronized"sv;
        case EL3HLT: return "level 3 halted"sv;
        case EL3RST: return "level 3 reset"sv;
        case ELNRNG: return "link number out of range"sv;
        case EUNATCH: return "protocol driver not attached"sv;
        case ENOCSI: return "no csi structure available"sv;
        case EL2HLT: return "level 2 halted"sv;
        case EBADE: return "invalid exchange"sv;
        case EBADR: return "invalid request descriptor"sv;
        case EXFULL: return "exchange full"sv;
        case ENOANO: return "no anode"sv;
        case EBADRQC: return "invalid request code"sv;
        case EBADSLT: return "invalid slot"sv;
        case EBFONT: return "bad font file format"sv;
        case ENOSTR: return "device not a stream"sv;
        case ENODATA: return "no data available"sv;
        case ETIME: return "timer expired"sv;
        case ENOSR: return "out of streams resources"sv;
        case ENONET: return "machine is not on the network"sv;
        case ENOPKG: return "package not installed"sv;
        case EREMOTE: return "object is remote"sv;
        case ENOLINK: return "link has been severed"sv;
        case EADV: return "advertise error"sv;
        case ESRMNT: return "srmount error"sv;
        case ECOMM: return "communication error on send"sv;
        case EPROTO: return "protocol error"sv;
        case EMULTIHOP: return "multihop attempted"sv;
        case EDOTDOT: return "rfs specific error"sv;
        case EBADMSG: return "not a data message"sv;
        case EOVERFLOW: return "value too large for defined data type"sv;
        case ENOTUNIQ: return "name not unique on network"sv;
        case EBADFD: return "file descriptor in bad state"sv;
        case EREMCHG: return "remote address changed"sv;
        case ELIBACC: return "can not access a needed shared library"sv;
        case ELIBBAD: return "accessing a corrupted shared library"sv;
        case ELIBSCN: return ".lib section in a.out corrupted"sv;
        case ELIBMAX: return "attempting to link in too many shared libraries"sv;
        case ELIBEXEC: return "cannot exec a shared library directly"sv;
        case EILSEQ: return "illegal byte sequence"sv;
        case ERESTART: return "interrupted system call should be restarted"sv;
        case ESTRPIPE: return "streams pipe error"sv;
        case EUSERS: return "too many users"sv;
        case ENOTSOCK: return "socket operation on non-socket"sv;
        case EDESTADDRREQ: return "destination address required"sv;
        case EMSGSIZE: return "message too long"sv;
        case EPROTOTYPE: return "protocol wrong type for socket"sv;
        case ENOPROTOOPT: return "protocol not available"sv;
        case EPROTONOSUPPORT: return "protocol not supported"sv;
        case ESOCKTNOSUPPORT: return "socket type not supported"sv;
        case EOPNOTSUPP: return "operation not supported on transport endpoint"sv;
        case EPFNOSUPPORT: return "protocol family not supported"sv;
        case EAFNOSUPPORT: return "address family not supported by protocol"sv;
        case EADDRINUSE: return "address already in use"sv;
        case EADDRNOTAVAIL: return "cannot assign requested address"sv;
        case ENETDOWN: return "network is down"sv;
        case ENETUNREACH: return "network is unreachable"sv;
        case ENETRESET: return "network dropped connection because of reset"sv;
        case ECONNABORTED: return "software caused connection abort"sv;
        case ECONNRESET: return "connection reset by peer"sv;
        case ENOBUFS: return "no buffer space available"sv;
        case EISCONN: return "transport endpoint is already connected"sv;
        case ENOTCONN: return "transport endpoint is not connected"sv;
        case ESHUTDOWN: return "cannot send after transport endpoint shutdown"sv;
        case ETOOMANYREFS: return "too many references: cannot splice"sv;
        case ETIMEDOUT: return "connection timed out"sv;
        case ECONNREFUSED: return "connection refused"sv;
        case EHOSTDOWN: return "host is down"sv;
        case EHOSTUNREACH: return "no route to host"sv;
        case EALREADY: return "operation already in progress"sv;
        case EINPROGRESS: return "operation now in progress"sv;
        case ESTALE: return "stale nfs file handle"sv;
        case EUCLEAN: return "structure needs cleaning"sv;
        case ENOTNAM: return "not a xenix named type file"sv;
        case ENAVAIL: return "no xenix semaphores available"sv;
        case EISNAM: return "is a named type file"sv;
        case EREMOTEIO: return "remote i/o error"sv;
        case EDQUOT: return "quota exceeded"sv;
        case ENOMEDIUM: return "no medium found"sv;
        case EMEDIUMTYPE: return "wrong medium type"sv;
        case ECANCELED: return "operation canceled"sv;
        case ENOKEY: return "required key not available"sv;
        case EKEYEXPIRED: return "key has expired"sv;
        case EKEYREVOKED: return "key has been revoked"sv;
        case EKEYREJECTED: return "key was rejected by service"sv;
        case EOWNERDEAD: return "owner died"sv;
        case ENOTRECOVERABLE: return "state not recoverable"sv;
        default: return "unknown error"sv;
        }
    }

    [[nodiscard]]
    std::string info() const {
        auto str   = to_string();
        auto descr = description();

        std::string result;
        result.reserve(str.size() + descr.size() + 3);

        result += str;
        result += " (";
        result += description();
        result += ')';

        return result;
    }

    friend std::ostream& operator<<(std::ostream& os, const errc& err) {
        return os << err.to_string();
    }

    operator int() const {
        return code;
    }

    int code = 0;
};

} // namespace dfdh
