#!/bin/bash
# Cross-compile the game for Windows (x86_64) using MinGW-w64 from Linux.
#
#   ./build-windows.sh                release build -> easybuild-windows/
#   ./build-windows.sh --debug        debug build   -> easybuild-windows-debug/
#   ./build-windows.sh prod           release build + self-contained dist dir
#   ./build-windows.sh --rebuild-subs force submodule rebuild
#   ./build-windows.sh dfdh           build only the named target
#             cfg_test|tasks_test|net_test
#
# Produces Windows executables under <builddir>/:
#   dfdh.exe, cfg_test.exe, tasks_test.exe, net_test.exe
# with <builddir>/dist ready to run on a Windows machine (dfdh.exe + data/
# + fs.cfg) when invoked with 'prod'.
#
# Notes:
# - SFML 3 is cross-built via its own CMake (Win32 backend); it fetches and
#   builds its dependencies as static libs (freetype, harfbuzz, FLAC, ogg,
#   vorbis, mbedTLS, libssh2). Audio goes through miniaudio: no OpenAL DLL.
# - LuaJIT 2.1 is built with `make CROSS=... TARGET_SYS=Windows` (official
#   cross-compilation path from its src/Makefile).
# - GLEW is replaced by a generated shim (3rd/include/GL/glew.h +
#   libglew32.a): the game only calls glewInit() and a fixed set of GL 3.x
#   entry points that Windows' <GL/gl.h> does not declare; the shim
#   resolves them at runtime via wglGetProcAddress.

CROSS=x86_64-w64-mingw32-
CXX_COMPILER=${CXX_COMPILER:-"${CROSS}g++"}
CC_COMPILER=${CC_COMPILER:-"${CROSS}gcc"}

colored() {
    if [ -t 1 ]; then
        local black="0;30" red="1;31"     green="1;32"  orange="0;33"
        local blue="1;34"  purple="0;35"  cyan="0;36"  lgray="0;37"
        local lred="1;31"  lgreen="1;32"  yellow="1;33" white="1;37"
        echo -en "\e[${!1}m$2\e[0m"
    else
        echo -n "$2"
    fi
    [ -z "$3" ] && echo
}

fail() {
    colored red "$1" 1>&2
    exit 1
}

is_commands_equal() {
    local cmd="$1"
    local path="$2"
    [ "command=\"$cmd\"" == "$(grep 'command=' "$path" 2>/dev/null)" ]
}

make_includes() {
    local IFS=$'\n'
    for include in ${1//;/$'\n'}; do
        local arg="-I"
        [ "${include%:*}" = "system" ] && arg="-isystem"
        printf '%s ' "$arg \"${include##*:}\""
    done
}

include_list() {
    while [ ! -z "$1" ]; do
        printf '%s;' "$1"
        shift
    done
}

build_static_lib() {
    local name="$1"
    local build_source="$2"
    local builddir="$3"
    local CXX_COMPILER="$4"
    local debug="$5"
    local flags="$6"
    local libs="$7"
    local includes="$8"

    # No TARGET check: the libraries are dependencies of the targets

    local cmd="$(command -v "$CXX_COMPILER") -c -std=c++20 \
        $([ "$debug" == true ] && echo "-O0 -g" || echo "-O2") \
        $flags \
        $(make_includes "$includes") \
        "$build_source" \
        -o "$builddir/$name.o" \
        $libs"

    local cmd_path="$builddir/.ezbld/$name-command"
    local already_built=false
    # No header dependency tracking: the source's directory (its local headers) must not be newer
    [ -f "$builddir/lib$name.a" ] && is_commands_equal "$cmd" "$cmd_path" && \
        [ -z "$(find "$(dirname "$build_source")" -maxdepth 1 -type f -newer "$builddir/lib$name.a" | head -1)" ] && \
        already_built=true

    if [ $already_built = true ]; then
        colored lpurple "lib$name.a already built"
        return 0
    fi

    colored yellow "Building lib$name.a ... " n
    eval $cmd || fail "Build failed"
    x86_64-w64-mingw32-ar rcs "$builddir/lib$name.a" "$builddir/$name.o" || fail "Build failed"
    rm -f "$builddir/$name.o"
    printf '%s\n%s\n' "source=\"$build_source\"" "command=\"$cmd\"" > "$cmd_path"
    colored lgreen "done"
}

build_executable() {
    local name="$1"
    local build_source="$2"
    local builddir="$3"
    local CXX_COMPILER="$4"
    local debug="$5"
    local flags="$6"
    local libs="$7"
    local includes="$8"

    [ -z "$TARGET" ] || [ "$TARGET" = "$name" ] || return 1

    local cmd="$(command -v "$CXX_COMPILER") -std=c++20 \
        $([ "$debug" == true ] && echo "-O0 -g" || echo "-O2") \
        $flags \
        -L "$builddir/3rd/lib" \
        -L "$builddir/luajit/lib" \
        -L "$builddir" \
        $(make_includes "$includes") \
        "$build_source" \
        -o "$builddir/$name.exe" \
        $libs"

    local cmd_path="$builddir/.ezbld/$name-command"
    local already_built=false
    [ -f "$builddir/$name.exe" ] && is_commands_equal "$cmd" "$cmd_path" && already_built=true

    if [ $already_built = true ]; then
        colored lpurple "$name.exe already built"
        return 1
    fi

    colored yellow "Building $name.exe ... " n
    eval $cmd || fail "Build failed"
    printf '%s\n%s\n' "source=\"$build_source\"" "command=\"$cmd\"" > "$cmd_path"
    colored lgreen "done"
    return 0
}

# cmake subproject: configure + build + install, cached by marker file
build_cmake_submodule() {
    local builddir="$1"
    local name="$2"
    shift 2

    local marker="$builddir/.ezbld/$name-done"
    # The submodule commit and the flags: an updated submodule is rebuilt
    local marker_key
    marker_key="$(git -C "submodules/$name" rev-parse HEAD) $*"
    if [ "$(cat "$marker" 2>/dev/null)" = "$marker_key" ] && [ "$rebuild_submodules" = false ]; then
        colored lpurple "Submodule '$name' already built"
        return 0
    fi

    colored yellow "Building submodule '$name' ..." n
    cmake -S "submodules/$name" -B "$builddir/submodules/$name" \
        -DCMAKE_TOOLCHAIN_FILE="$(pwd)/CMake/mingw-w64.cmake" \
        -DCMAKE_BUILD_TYPE=$([ "$debug" == true ] && echo Debug || echo Release) \
        -DCMAKE_INSTALL_PREFIX="$builddir/3rd" \
        "$@" > "$builddir/.ezbld/$name-cmake.log" 2>&1 || \
        fail "CMake configure for $name failed (see $builddir/.ezbld/$name-cmake.log)"
    cmake --build "$builddir/submodules/$name" -j"$JOBS" >> "$builddir/.ezbld/$name-cmake.log" 2>&1 || \
        fail "Build for $name failed (see $builddir/.ezbld/$name-cmake.log)"
    cmake --install "$builddir/submodules/$name" >> "$builddir/.ezbld/$name-cmake.log" 2>&1 || \
        fail "Install for $name failed (see $builddir/.ezbld/$name-cmake.log)"
    printf '%s' "$marker_key" > "$marker"
    colored lgreen "done"
}

# Minimal GLEW replacement.
# The game uses <GL/glew.h> for glewInit() and GL entry points up to 3.0.
# MinGW's <GL/gl.h> only declares GL 1.1, so we generate a header with the
# prototypes of every function the game uses above 1.1 plus a small library
# that resolves them at runtime via wglGetProcAddress — no GLEW binary
# needed (SFML's WGL context loads the GL driver before glewInit() runs).
# GL 1.1 entry points stay direct opengl32.dll imports.
build_glew_shim() {
    local builddir="$1"
    local flags="$2"
    local inc="$builddir/3rd/include/GL"
    mkdir -p "$inc" "$builddir/.ezbld"

    cat > "$inc/glew.h" <<'EOF'
/* Minimal GLEW-compatible shim generated by build-windows.sh.
 * Declares the GL >= 1.2 entry points the project uses (MinGW's <GL/gl.h>
 * only declares GL 1.1) and resolves them at runtime via
 * wglGetProcAddress (see glew_shim.cpp in the build dir). */
#ifndef GLEW_SHIM_H
#define GLEW_SHIM_H

#include <GL/gl.h>

/* MinGW's gl.h doesn't define these */
#ifndef GLAPI
#define GLAPI
#endif
#ifndef GLAPIENTRY
#define GLAPIENTRY
#endif
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;

/* Constants the project uses that are absent from MinGW's GL 1.1 gl.h */
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_FUNC_ADD
#define GL_FUNC_ADD 0x8006
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#endif
#ifndef GL_STREAM_DRAW
#define GL_STREAM_DRAW 0x88E0
#endif
#ifndef GL_WRITE_ONLY
#define GL_WRITE_ONLY 0x88E1
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* GL 1.1 entry points missing from MinGW's gl.h */
GLAPI GLint GLAPIENTRY glGetUniformLocation(GLuint program, const GLchar* name);
GLAPI void  GLAPIENTRY glUniform1i(GLuint location, GLint v);
GLAPI void  GLAPIENTRY glUniformMatrix4fv(GLuint location, GLsizei count, GLboolean transpose, const GLfloat* value);

/* GL 1.3 / 1.4 */
GLAPI void  GLAPIENTRY glActiveTexture(GLenum texture);
GLAPI void  GLAPIENTRY glBlendEquation(GLenum mode);

/* GL 1.5 */
GLAPI void  GLAPIENTRY glGenBuffers(GLsizei n, GLuint* buffers);
GLAPI void  GLAPIENTRY glDeleteBuffers(GLsizei n, const GLuint* buffers);
GLAPI void  GLAPIENTRY glBindBuffer(GLenum target, GLuint buffer);
GLAPI void  GLAPIENTRY glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage);
GLAPI void* GLAPIENTRY glMapBuffer(GLenum target, GLenum access);
GLAPI GLboolean GLAPIENTRY glUnmapBuffer(GLenum target);

/* GL 2.0 */
GLAPI void  GLAPIENTRY glDeleteProgram(GLuint program);
GLAPI void  GLAPIENTRY glEnableVertexAttribArray(GLuint index);
GLAPI void  GLAPIENTRY glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer);
GLAPI GLint GLAPIENTRY glGetAttribLocation(GLuint program, const GLchar* name);

/* GL 3.0 */
GLAPI GLuint  GLAPIENTRY glCreateProgram(void);
GLAPI GLuint  GLAPIENTRY glCreateShader(GLenum type);
GLAPI void    GLAPIENTRY glDeleteShader(GLuint shader);
GLAPI void    GLAPIENTRY glAttachShader(GLuint program, GLuint shader);
GLAPI void    GLAPIENTRY glDetachShader(GLuint program, GLuint shader);
GLAPI void    GLAPIENTRY glCompileShader(GLuint shader);
GLAPI void    GLAPIENTRY glLinkProgram(GLuint program);
GLAPI void    GLAPIENTRY glUseProgram(GLuint program);
GLAPI void    GLAPIENTRY glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length);
GLAPI void    GLAPIENTRY glGetShaderiv(GLuint shader, GLenum pname, GLint* params);
GLAPI void    GLAPIENTRY glGetProgramiv(GLuint program, GLenum pname, GLint* params);
GLAPI void    GLAPIENTRY glCreateVertexArrays(GLsizei n, GLuint* arrays);
GLAPI void    GLAPIENTRY glDeleteVertexArrays(GLsizei n, const GLuint* arrays);
GLAPI void    GLAPIENTRY glBindVertexArray(GLuint array);
GLAPI void    GLAPIENTRY glGenVertexArrays(GLsizei n, GLuint* arrays);
GLAPI GLboolean GLAPIENTRY glIsVertexArray(GLuint array);

/* missing from MinGW's gl.h */
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_CONTEXT_PROFILE_MASK 0x9146
GLAPI void    GLAPIENTRY glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
GLAPI void    GLAPIENTRY glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
typedef long long GLintptr;
GLAPI void    GLAPIENTRY glGetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, void* data);

/* GLEW API surface actually used by the project */
typedef int GLError;
GLAPI GLboolean GLAPIENTRY glewInit(void);
GLAPI GLError   GLAPIENTRY glewGetError(void);

#define GLEW_OK 0
#define GLEW_VERSION_3_0 1
#define GLEW_VERSION_3_1 1
#define GLEW_VERSION_3_2 1
#define GLEW_VERSION_3_3 1

#ifdef __cplusplus
}
#endif

#endif /* GLEW_SHIM_H */
EOF

    cat > "$builddir/glew_shim.cpp" <<'EOF'
/* GLEW shim implementation (generated by build-windows.sh).
 * Resolves GL >= 1.2 entry points at first use via wglGetProcAddress. */
#include <GL/glew.h>
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>

/*
 * Resolution cache keyed by entry point name. (A single per-template-type static
 * would alias every entry point with the same signature to whichever of them
 * was resolved first: glCompileShader/glLinkProgram/glUseProgram/... all share
 * one void(*)(GLuint) type and must not share one pointer.)
 */
static void* gl_resolve_ptr(const char* name) {
    static std::unordered_map<std::string, void*> cache;
    auto pos = cache.find(name);
    if (pos != cache.end())
        return pos->second;
    PROC p = nullptr;
    /*
     * wglGetProcAddress only works with a current GL context, and some drivers
     * (e.g. the Intel GL-on-DX bridge) return NULL intermittently. Retry a while
     * instead of failing on the first miss.
     */
    for (int i = 0; i < 100 && !p; ++i) {
        p = wglGetProcAddress(name);
        if (!p)
            Sleep(10);
    }
    if (!p) {
        std::fprintf(stderr, "GLEW shim: failed to resolve %s\n", name);
        std::abort();
    }
    void* fp = reinterpret_cast<void*>(p);
    cache[name] = fp;
    return fp;
}

template <typename F>
static F gl_resolved(const char* name) {
    return reinterpret_cast<F>(gl_resolve_ptr(name));
}

extern "C" {

GLint   glGetUniformLocation(GLuint p, const GLchar* n) { return gl_resolved<GLint (*)(GLuint, const GLchar*)>("glGetUniformLocation")(p, n); }
void    glUniform1i(GLuint l, GLint v) { gl_resolved<void (*)(GLuint, GLint)>("glUniform1i")(l, v); }
void    glUniformMatrix4fv(GLuint l, GLsizei c, GLboolean t, const GLfloat* v) {
    gl_resolved<void (*)(GLuint, GLsizei, GLboolean, const GLfloat*)>("glUniformMatrix4fv")(l, c, t, v);
}

void    glActiveTexture(GLenum t) { gl_resolved<void (*)(GLenum)>("glActiveTexture")(t); }
void    glBlendEquation(GLenum m) { gl_resolved<void (*)(GLenum)>("glBlendEquation")(m); }
void    glGenBuffers(GLsizei n, GLuint* b) { gl_resolved<void (*)(GLsizei, GLuint*)>("glGenBuffers")(n, b); }
void    glDeleteBuffers(GLsizei n, const GLuint* b) { gl_resolved<void (*)(GLsizei, const GLuint*)>("glDeleteBuffers")(n, b); }
void    glBindBuffer(GLenum t, GLuint b) { gl_resolved<void (*)(GLenum, GLuint)>("glBindBuffer")(t, b); }
void    glBufferData(GLenum t, GLsizeiptr s, const void* d, GLenum u) { gl_resolved<void (*)(GLenum, GLsizeiptr, const void*, GLenum)>("glBufferData")(t, s, d, u); }
void*   glMapBuffer(GLenum t, GLenum a) { return gl_resolved<void* (*)(GLenum, GLenum)>("glMapBuffer")(t, a); }
GLboolean glUnmapBuffer(GLenum t) { return gl_resolved<GLboolean (*)(GLenum)>("glUnmapBuffer")(t); }
void    glDeleteProgram(GLuint p) { gl_resolved<void (*)(GLuint)>("glDeleteProgram")(p); }
void    glEnableVertexAttribArray(GLuint i) { gl_resolved<void (*)(GLuint)>("glEnableVertexAttribArray")(i); }
void    glVertexAttribPointer(GLuint i, GLint s, GLenum t, GLboolean n, GLsizei st, const void* p) {
    gl_resolved<void (*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)>("glVertexAttribPointer")(i, s, t, n, st, p);
}
GLint   glGetAttribLocation(GLuint p, const GLchar* n) { return gl_resolved<GLint (*)(GLuint, const GLchar*)>("glGetAttribLocation")(p, n); }

GLuint  glCreateProgram(void) { return gl_resolved<GLuint (*)(void)>("glCreateProgram")(); }
GLuint  glCreateShader(GLenum type) { return gl_resolved<GLuint (*)(GLenum)>("glCreateShader")(type); }
void    glDeleteShader(GLuint s) { gl_resolved<void (*)(GLuint)>("glDeleteShader")(s); }
void    glAttachShader(GLuint p, GLuint s) { gl_resolved<void (*)(GLuint, GLuint)>("glAttachShader")(p, s); }
void    glDetachShader(GLuint p, GLuint s) { gl_resolved<void (*)(GLuint, GLuint)>("glDetachShader")(p, s); }
void    glCompileShader(GLuint s) { gl_resolved<void (*)(GLuint)>("glCompileShader")(s); }
void    glLinkProgram(GLuint p) { gl_resolved<void (*)(GLuint)>("glLinkProgram")(p); }
void    glUseProgram(GLuint p) { gl_resolved<void (*)(GLuint)>("glUseProgram")(p); }
void    glShaderSource(GLuint s, GLsizei c, const GLchar* const* str, const GLint* len) {
    gl_resolved<void (*)(GLuint, GLsizei, const GLchar* const*, const GLint*)>("glShaderSource")(s, c, str, len);
}
void    glGetShaderiv(GLuint s, GLenum p, GLint* v) { gl_resolved<void (*)(GLuint, GLenum, GLint*)>("glGetShaderiv")(s, p, v); }
void    glGetProgramiv(GLuint p, GLenum pn, GLint* v) { gl_resolved<void (*)(GLuint, GLenum, GLint*)>("glGetProgramiv")(p, pn, v); }
void    glCreateVertexArrays(GLsizei n, GLuint* a) { gl_resolved<void (*)(GLsizei, GLuint*)>("glCreateVertexArrays")(n, a); }
void    glDeleteVertexArrays(GLsizei n, const GLuint* a) { gl_resolved<void (*)(GLsizei, const GLuint*)>("glDeleteVertexArrays")(n, a); }
void    glBindVertexArray(GLuint a) { gl_resolved<void (*)(GLuint)>("glBindVertexArray")(a); }
void    glGenVertexArrays(GLsizei n, GLuint* a) { gl_resolved<void (*)(GLsizei, GLuint*)>("glGenVertexArrays")(n, a); }
GLboolean glIsVertexArray(GLuint a) { return gl_resolved<GLboolean (*)(GLuint)>("glIsVertexArray")(a); }
void    glGetShaderInfoLog(GLuint s, GLsizei b, GLsizei* l, GLchar* i) {
    gl_resolved<void (*)(GLuint, GLsizei, GLsizei*, GLchar*)>("glGetShaderInfoLog")(s, b, l, i);
}
void    glGetProgramInfoLog(GLuint p, GLsizei b, GLsizei* l, GLchar* i) {
    gl_resolved<void (*)(GLuint, GLsizei, GLsizei*, GLchar*)>("glGetProgramInfoLog")(p, b, l, i);
}
void    glGetBufferSubData(GLenum t, GLintptr o, GLsizeiptr s, void* d) {
    gl_resolved<void (*)(GLenum, GLintptr, GLsizeiptr, void*)>("glGetBufferSubData")(t, o, s, d);
}

/* SFML already loaded the GL driver via glad when it created the WGL
 * context, so there is nothing left to initialize. */
GLboolean glewInit(void) { return 1; }
GLError glewGetError(void) { return GLEW_OK; }

} // extern "C"
EOF

    local cmd="$CXX_COMPILER -c -O2 -I $builddir/3rd/include $builddir/glew_shim.cpp -o $builddir/glew_shim.o"
    local cmd_path="$builddir/.ezbld/glew_shim-command"
    if [ ! -f "$builddir/libglew32.a" ] || ! is_commands_equal "$cmd" "$cmd_path"; then
        colored yellow "Building GLEW shim ... " n
        eval $cmd || fail "GLEW shim build failed"
        x86_64-w64-mingw32-ar rcs "$builddir/libglew32.a" "$builddir/glew_shim.o"
        rm -f "$builddir/glew_shim.o"
        printf '%s\n%s\n' "source=glew_shim.cpp" "command=\"$cmd\"" > "$cmd_path"
        colored lgreen "done"
    else
        colored lpurple "GLEW shim already built"
    fi
}

# LuaJIT 2.1 cross build (official cross-compilation path from src/Makefile)
build_luajit() {
    local builddir="$1"
    local marker="$builddir/.ezbld/luajit-done"
    if [ -f "$marker" ] && [ "$rebuild_submodules" = false ]; then
        colored lpurple "LuaJIT already built"
        return 0
    fi

    colored yellow "Building LuaJIT 2.1 ... " n
    (
        cd submodules/luacpp/submodules/LuaJIT/src || exit 1
        make clean > /dev/null 2>&1
        make -j"$JOBS" CROSS=$CROSS TARGET_SYS=Windows BUILDMODE=static || exit 1
    ) > "$builddir/.ezbld/luajit-make.log" 2>&1 || \
        fail "LuaJIT build failed (see $builddir/.ezbld/luajit-make.log)"

    mkdir -p "$builddir/luajit/lib" "$builddir/3rd/include/luajit-2.1"
    # mingw make produces libluajit.a; name it like the Linux build (-l name stays the same)
    cp submodules/luacpp/submodules/LuaJIT/src/libluajit.a "$builddir/luajit/lib/libluajit-5.1.a" || fail "libluajit.a not found"
    cp submodules/luacpp/submodules/LuaJIT/src/{lua.h,lua.hpp,lauxlib.h,lualib.h,luajit.h,luaconf.h} \
        "$builddir/3rd/include/luajit-2.1/" || fail "LuaJIT headers not found"
    # The Linux build (luacpp) copies this source tree: the Windows objects
    # would be taken as up to date there
    make -C submodules/luacpp/submodules/LuaJIT/src clean > /dev/null 2>&1
    touch "$marker"
    colored lgreen "done"
}

# Generated headers (src/*_generated.hpp, like build-plz's codegen_step):
# the generator runs on the build host, the SFML header is preprocessed for
# the target
codegen_step() {
    local builddir="$1"
    local input_header="$2"
    local generator_name="$3"
    local generator_cpp="$4"
    local output_header="$5"
    local host_cxx="${HOST_CXX_COMPILER:-g++}"

    command -v "$host_cxx" > /dev/null || fail "$host_cxx not found (host compiler for $generator_name)"

    if [ -f "$output_header" ] && [ "$output_header" -nt "$generator_cpp" ] && \
        [ "$output_header" -nt "$input_header" ]; then
        colored lpurple "$output_header already generated"
        return 0
    fi

    colored yellow "Generating $output_header ... " n
    "$host_cxx" -std=c++20 -O1 "$generator_cpp" -o "$builddir/$generator_name" || \
        fail "Build of $generator_name failed"
    "$CXX_COMPILER" -E -I "$builddir/3rd/include" "$input_header" | "$builddir/$generator_name" \
        > "$output_header.tmp" && [ "${PIPESTATUS[0]}" = 0 ] || fail "Generation of $output_header failed"
    mv "$output_header.tmp" "$output_header"
    colored lgreen "done"
}

build_dist() {
    local builddir="$1"
    colored yellow "Assembling dist ... " n
    rm -rf "$builddir/dist"
    mkdir -p "$builddir/dist"
    cp "$builddir/dfdh.exe" "$builddir/dist/"
    cp -r data "$builddir/dist/"
    cp fs.cfg "$builddir/dist/"
    colored lgreen "done ($builddir/dist)"
}

_build() {
    local builddir='./easybuild-windows'
    local rebuild_submodules=false
    local debug=false
    local define_flags=
    local prod=false
    local TARGET=
    local JOBS

    while [ -n "$1" ]; do
        case "$1" in
        --builddir=*)
            builddir="${1#*=}"
            [ "$debug" = true ] && builddir="$builddir-debug"
            ;;
        --rebuild-subs)
            rebuild_submodules=true
            ;;
        --debug)
            debug=true
            builddir="$builddir-debug"
            ;;
        --define=*)
            local dval="${1#*=}"
            [[ -n "$dval" && "$dval" =~ ^[A-Za-z_][A-Za-z_0-9]*(=[^ ]*)?$ ]] || \
                fail "Invalid --define value: '$dval'"
            define_flags+=" -D$dval"
            ;;
        --help)
            sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        --*)
            fail "Invalid option: '$1'"
            ;;
        prod)
            prod=true
            TARGET="dfdh"
            ;;
        *)
            TARGET+="$1"
            ;;
        esac
        shift
    done

    command -v "$CC_COMPILER" > /dev/null || fail "$CC_COMPILER not found (apt install g++-mingw-w64-x86-64)"
    command -v "$CXX_COMPILER" > /dev/null || fail "$CXX_COMPILER not found"
    JOBS=${JOBS:-$(nproc)}

    local hardening_flags="
-fstrict-aliasing
-Wall
-Wextra
-Wpedantic
-Wcast-align
-Wconversion
-Wctor-dtor-privacy
-Wextra-semi
-Wfloat-equal
-Wnon-virtual-dtor
-Wold-style-cast
-Woverloaded-virtual
-Wredundant-decls
-Wsign-conversion
-Wsign-promo
-Werror=unused-result
-Wno-unused-function
-Wno-missing-braces
"

    local static_flags="-static-libgcc -static-libstdc++"

# On Windows M_PI & friends only exist in <math.h> with this define.
# SFML_STATIC stops SFML headers from marking symbols __declspec(dllimport)
# when we link its static libraries.
local platform_defines=" -D_USE_MATH_DEFINES -DSFML_STATIC"

    # winpthreads (miniaudio) is linked statically: no libwinpthread-1.dll in the dist
    local system_libs="
-lglew32 -lopengl32 -lwinmm -lgdi32 -lws2_32 -lkernel32 -ldinput -ldxguid -ldxerr8 -lshell32 -lwinspool -luser32
-ldnsapi -lcrypt32 -lbcrypt -lole32
-Wl,-Bstatic -lpthread -Wl,-Bdynamic
-lm
"

    local static_libs="-Wl,--start-group \
$builddir/libnuklear.a \
$builddir/libglew32.a \
$builddir/3rd/lib/libsfml-system-s.a \
$builddir/3rd/lib/libsfml-window-s.a \
$builddir/3rd/lib/libsfml-graphics-s.a \
$builddir/3rd/lib/libsfml-network-s.a \
$builddir/3rd/lib/libsfml-audio-s.a \
$builddir/3rd/lib/libfreetype.a \
$builddir/3rd/lib/libharfbuzz.a \
$builddir/3rd/lib/libFLAC.a \
$builddir/3rd/lib/libvorbisenc.a \
$builddir/3rd/lib/libvorbisfile.a \
$builddir/3rd/lib/libvorbis.a \
$builddir/3rd/lib/libogg.a \
$builddir/3rd/lib/libssh2.a \
$builddir/3rd/lib/libmbedtls.a \
$builddir/3rd/lib/libmbedx509.a \
$builddir/3rd/lib/libmbedcrypto.a \
$builddir/luajit/lib/libluajit-5.1.a \
-Wl,--end-group"

    mkdir -p "$builddir/3rd" "$builddir/.ezbld" "$builddir/luajit"

    build_cmake_submodule "$builddir" SFML \
        -DBUILD_SHARED_LIBS=OFF \
        -DSFML_BUILD_EXAMPLES=OFF

    build_cmake_submodule "$builddir" Catch2 \
        -DCATCH_BUILD_TESTING=OFF \
        -DCATCH_INSTALL_DOCS=OFF \
        -DCATCH_INSTALL_EXTRAS=OFF

    build_luajit "$builddir"
    build_glew_shim "$builddir" "$hardening_flags"

    codegen_step "$builddir" "$builddir/3rd/include/SFML/Window/Keyboard.hpp" \
        sfml_key_generator src/key_generator.cpp src/key_generated.hpp
    codegen_step "$builddir" "$builddir/3rd/include/SFML/Window/Event.hpp" \
        sfml_event_type_generator src/event_type_generator.cpp src/event_type_generated.hpp

    # Header-only / single-file submodules
    # Copied again when the submodule is updated
    cmp -s submodules/Nuklear/nuklear.h "$builddir/3rd/include/nuklear.h" || \
        cp submodules/Nuklear/nuklear.h "$builddir/3rd/include/" || fail "Can't copy nuklear.h"
    # source includes use <luaffi/...> (the luacpp submodule project name)
    mkdir -p "$builddir/3rd/include/luaffi"
    cp submodules/luacpp/src/*.hpp "$builddir/3rd/include/luaffi/"

    build_static_lib \
        nuklear \
        src/ui/nuklear.cpp \
        "$builddir" \
        "$CXX_COMPILER" \
        "$debug" \
        "$hardening_flags $platform_defines" \
        "" \
        "$(include_list \
            "system:$(pwd)/$builddir/3rd/include" \
        )"

    build_executable \
        dfdh \
        diefastdiehard.cpp \
        "$builddir" \
        "$CXX_COMPILER" \
        "$debug" \
        "$hardening_flags $static_flags $define_flags $platform_defines" \
        "$static_libs $system_libs" \
        "$(include_list \
            "system:$(pwd)/$builddir/3rd/include/luajit-2.1" \
            "system:$(pwd)/$builddir/3rd/include" \
            "$(pwd)/src" \
        )"

    build_executable \
        cfg_test \
        tests/cfg_test.cpp \
        "$builddir" \
        "$CXX_COMPILER" \
        "$debug" \
        "$hardening_flags $static_flags $define_flags $platform_defines" \
        "-L $builddir/3rd/lib -lCatch2Main -lCatch2 $system_libs" \
        "$(include_list \
            "system:$(pwd)/$builddir/3rd/include" \
            "$(pwd)/src" \
        )"

    build_executable \
        tasks_test \
        tests/tasks.cpp \
        "$builddir" \
        "$CXX_COMPILER" \
        "$debug" \
        "$hardening_flags $static_flags $define_flags $platform_defines" \
        "-L $builddir/3rd/lib -lCatch2Main -lCatch2 $system_libs" \
        "$(include_list \
            "system:$(pwd)/$builddir/3rd/include" \
            "$(pwd)/src" \
        )"

    build_executable \
        net_test \
        tests/net_test.cpp \
        "$builddir" \
        "$CXX_COMPILER" \
        "$debug" \
        "$hardening_flags $static_flags $define_flags $platform_defines" \
        "-L $builddir/3rd/lib -lCatch2Main -lCatch2 $static_libs $system_libs" \
        "$(include_list \
            "system:$(pwd)/$builddir/3rd/include/luajit-2.1" \
            "system:$(pwd)/$builddir/3rd/include" \
            "$(pwd)/src" \
        )"

    if [ "$prod" = true ]; then
        build_dist "$builddir"
    fi
}

_build "$@"
