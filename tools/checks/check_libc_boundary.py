#!/usr/bin/env python3
"""Reject direct libc and operating-system use in renderer sources.

The renderer reaches libc and operating-system services only through lib/.
The compiler enforces most of this boundary: renderer targets define
VKR_LIBC_BOUNDARY, so lib headers expose no stdio, stdlib, string, POSIX or
Win32 declarations. Two gaps remain, and this guard covers them: vkr_math.h
keeps <math.h> visible for its inline wrappers, and Objective-C translation
units import Foundation, which brings the whole C library with it.

Usage:
  python3 tools/checks/check_renderer_libc.py
"""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
RENDERER = ROOT / 'renderer' / 'src'
SUFFIXES = ('.c', '.h', '.m', '.inc')

# Headers a renderer source may include directly: freestanding C headers and
# the graphics APIs the backends program against.
ALLOWED_INCLUDES = {
    'stdarg.h', 'stdbool.h', 'stddef.h', 'stdint.h',
    'vulkan/vulkan.h', 'spirv_reflect.h',
}
# The Vulkan backend's window-system unit owns the Win32 surface types.
FILE_INCLUDES = {
    'vulkan/vkr_vulkan_wsi.c': {'windows.h', 'vulkan/vulkan_win32.h'},
}
METAL_INCLUDES = {
    'Metal/Metal.h', 'Foundation/Foundation.h', 'QuartzCore/CAMetalLayer.h',
    'QuartzCore/CATransaction.h', 'simd/simd.h', 'dispatch/dispatch.h',
    'Block.h',
}

MATH = '''
acos acosf acosh acoshf asin asinf asinh asinhf atan atanf atan2 atan2f atanh
atanhf cbrt cbrtf ceil ceilf copysign copysignf _copysignf cos cosf cosh coshf
erf erff exp expf exp2 exp2f expm1 expm1f fabs fabsf fdim fdimf floor floorf
fma fmaf fmax fmaxf fmin fminf fmod fmodf frexp frexpf hypot hypotf ilogb ldexp
ldexpf lgamma llrint llround log logf log10 log10f log1p log1pf log2 log2f logb
lrint lrintf lround lroundf modf modff nan nanf nearbyint nearbyintf nextafter
nextafterf pow powf remainder remainderf rint rintf round roundf scalbn scalbnf
sin sinf sinh sinhf sqrt sqrtf tan tanf tanh tanhf tgamma trunc truncf
isfinite isinf isnan isnormal signbit fpclassify INFINITY NAN HUGE_VAL
HUGE_VALF M_PI M_PI_2 M_PI_4 M_E M_SQRT2
'''
STRING = '''
memcpy memmove memset memcmp memchr strlen strnlen strcmp strncmp strcpy strncpy
strcat strncat strchr strrchr strstr strtok strdup strndup strerror strcasecmp
strncasecmp
'''
STDIO = '''
printf fprintf sprintf snprintf vprintf vfprintf vsprintf vsnprintf puts fputs
putchar fputc fopen freopen fclose fread fwrite fseek ftell rewind fflush fgets
fgetc getc ferror feof perror remove rename tmpfile FILE stdin stdout stderr
SEEK_SET SEEK_CUR SEEK_END
'''
STDLIB = '''
malloc calloc realloc free aligned_alloc posix_memalign qsort bsearch abort exit
_Exit quick_exit atexit getenv setenv unsetenv putenv system rand srand RAND_MAX
atoi atol atoll atof strtol strtoul strtoll strtoull strtod strtof abs labs
llabs div EXIT_SUCCESS EXIT_FAILURE
'''
OTHER_C = '''
isdigit isalpha isalnum isspace isupper islower isxdigit tolower toupper time
clock difftime mktime localtime gmtime strftime errno assert setjmp longjmp
signal raise
FLT_MAX FLT_MIN FLT_EPSILON FLT_TRUE_MIN DBL_MAX DBL_MIN DBL_EPSILON
PRId32 PRIu32 PRIx32 PRId64 PRIu64 PRIx64
'''
OPERATING_SYSTEM = '''
open close read write pread pwrite lseek fsync fdatasync ftruncate mkdir rmdir
unlink stat fstat lstat access getcwd chdir realpath mmap munmap getpid usleep
nanosleep sleep dlopen dlsym dlclose sysctl sysctlbyname clock_gettime
gettimeofday ssize_t off_t pid_t O_RDONLY O_WRONLY O_RDWR O_CREAT O_EXCL O_TRUNC
O_APPEND EINTR EIO ENOENT EEXIST ENAMETOOLONG EOVERFLOW
mach_absolute_time mach_timebase_info CACurrentMediaTime CFAbsoluteTimeGetCurrent
NSFileManager NSProcessInfo NSDate NSThread NSLog NSUserDefaults
NSTemporaryDirectory os_log os_unfair_lock_lock os_unfair_lock_unlock
Sleep GetTickCount QueryPerformanceCounter QueryPerformanceFrequency
GetModuleHandleW LoadLibraryW GetProcAddress GetLastError
dispatch_queue_create dispatch_sync dispatch_async dispatch_release
dispatch_once dispatch_group_create dispatch_group_async dispatch_group_wait
dispatch_group_notify dispatch_get_global_queue dispatch_get_main_queue
dispatch_semaphore_create dispatch_semaphore_wait dispatch_semaphore_signal
Block_copy Block_release QOS_CLASS_USER_INITIATED QOS_CLASS_DEFAULT
QOS_CLASS_UTILITY QOS_CLASS_BACKGROUND
'''

BANNED = {}
for group, words in (('math', MATH), ('string', STRING), ('stdio', STDIO),
                     ('stdlib', STDLIB), ('libc', OTHER_C),
                     ('platform', OPERATING_SYSTEM)):
    for word in words.split():
        BANNED[word] = group

# Metal API calls that take GCD and Blocks types, with no lib equivalent:
# MTLSharedEventListener needs a dispatch queue, and MTL4 commit feedback
# needs a Block handler drained through a serial queue.
METAL_API_GCD = {
    'dispatch_queue_create', 'dispatch_sync', 'dispatch_async',
    'dispatch_release', 'dispatch_once', 'Block_copy', 'Block_release',
}
EXCEPTIONS = {
    'metal/vkr_metal_packet_renderer.m': METAL_API_GCD,
    'metal/internal/vkr_metal_packet_present.inc': METAL_API_GCD,
    'metal/internal/vkr_metal_packet_setup.inc': METAL_API_GCD,
    'metal/internal/vkr_metal_packet_lifecycle.inc': METAL_API_GCD,
    'metal/internal/vkr_metal_packet_commands.inc': METAL_API_GCD,
}

# Types, macros and objects match any use; functions match only calls, since
# names such as read, write or remainder are also ordinary variables.
ANY_USE = {
    'FILE', 'stdin', 'stdout', 'stderr', 'errno', 'ssize_t', 'off_t', 'pid_t',
    'NSFileManager', 'NSProcessInfo', 'NSDate', 'NSThread', 'NSUserDefaults',
}

IDENTIFIER = re.compile(r'[A-Za-z_][A-Za-z0-9_]*')
INCLUDE = re.compile(r'^\s*#\s*(?:include|import)\s*<([^>]+)>')


def strip_comments_and_strings(text):
    """Blank comments and literals, keeping line structure for reporting."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', text[i:j]))
            i = j
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            out.append(c + ' ' * (j - i - 2) + c if j - i >= 2 else c)
            i = j
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def sources():
    for path in sorted(RENDERER.rglob('*')):
        relative = path.relative_to(RENDERER)
        if path.suffix not in SUFFIXES or not path.is_file():
            continue
        if relative.parts[0] == 'shaders' or path.name.endswith('_data.inc'):
            continue
        yield path, relative.as_posix()


def check(path, relative):
    failures = []
    raw = path.read_text(encoding='utf-8')
    allowed_includes = ALLOWED_INCLUDES | FILE_INCLUDES.get(relative, set()) | (
        METAL_INCLUDES if relative.startswith('metal/') else set())
    for number, line in enumerate(raw.splitlines(), 1):
        match = INCLUDE.match(line)
        if match and match.group(1) not in allowed_includes:
            failures.append(f'renderer/src/{relative}:{number}: '
                            f'<{match.group(1)}> is outside the lib boundary')
    exceptions = EXCEPTIONS.get(relative, set())
    text = strip_comments_and_strings(raw)
    for number, line in enumerate(text.splitlines(), 1):
        if INCLUDE.match(line) or line.lstrip().startswith('#'):
            continue
        for match in IDENTIFIER.finditer(line):
            word = match.group(0)
            group = BANNED.get(word)
            if not group or word in exceptions:
                continue
            before = line[:match.start()].rstrip()
            # Members, Objective-C selectors and designated fields share
            # names such as close or write with the C library.
            if before.endswith(('.', '->')):
                continue
            after = line[match.end():].lstrip()
            if after.startswith(':') and not after.startswith('::'):
                continue
            is_macro = word.isupper() or word.startswith(('M_', 'PRI'))
            if not is_macro and word not in ANY_USE and \
                    not after.startswith('('):
                continue
            failures.append(f'renderer/src/{relative}:{number}: {word} '
                            f'({group}) has a lib owner')
    return failures


def main():
    failures = []
    for path, relative in sources():
        failures.extend(check(path, relative))
    if failures:
        print('\n'.join(failures))
        print(f'check_renderer_libc: {len(failures)} direct libc or platform '
              'uses in renderer/src; use the lib API')
        return 1
    print('Renderer libc boundary guard passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
