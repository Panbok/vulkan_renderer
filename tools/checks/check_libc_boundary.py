#!/usr/bin/env python3
"""Reject direct libc and operating-system use in renderer and runtime sources.

The renderer and runtime reach libc and operating-system services only through
lib/ (ADR-118). The compiler enforces most of this boundary: their targets
define VKR_LIBC_BOUNDARY, so lib headers expose no stdio, stdlib, string,
POSIX or Win32 declarations. This guard covers what the compiler cannot:
vkr_math.h keeps <math.h> visible for its inline wrappers, Objective-C units
import Foundation, vendor headers such as stb_image.h include stdio, and C++
units see the C library through the C++ standard library.

Usage:
  python3 tools/checks/check_libc_boundary.py
"""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
SUFFIXES = ('.c', '.h', '.m', '.inc', '.cpp')

# C library and operating-system headers. Freestanding headers (stdarg.h,
# stdbool.h, stddef.h, stdint.h), compiler intrinsics, graphics APIs, vendor
# libraries and the C++ standard library outside its C wrappers stay allowed.
C_LIBRARY_HEADERS = set('''
assert.h complex.h ctype.h errno.h fenv.h float.h inttypes.h limits.h locale.h
math.h setjmp.h signal.h stdatomic.h stdio.h stdlib.h string.h tgmath.h
threads.h time.h uchar.h wchar.h wctype.h
cassert cctype cerrno cfenv cfloat cinttypes climits clocale cmath csetjmp
csignal cstdio cstdlib cstring ctime cuchar cwchar cwctype
'''.split())
OS_HEADER = re.compile(
    r'^(unistd\.h|fcntl\.h|pthread\.h|dlfcn\.h|dirent\.h|poll\.h|sched\.h|'
    r'sys/|mach/|mach-o/|os/|windows\.h|windowsx\.h|io\.h|shellapi\.h|'
    r'shlobj\.h|shobjidl\.h|objbase\.h|combaseapi\.h|dxgi|xinput\.h|'
    r'winsock|ws2tcpip\.h|timeapi\.h|Block\.h|dispatch/|TargetConditionals\.h|'
    r'(AppKit|Cocoa|CoreFoundation|CoreGraphics|Foundation|GameController|'
    r'Metal|QuartzCore|UniformTypeIdentifiers|simd)/)')

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
strncasecmp strpbrk strspn strcspn
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
FLT_RADIX FLT_MANT_DIG FLT_MAX_EXP CHAR_BIT INT_MAX INT_MIN UINT_MAX LONG_MAX
LONG_MIN LLONG_MAX ULONG_MAX SHRT_MAX USHRT_MAX
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
METAL_HEADER = re.compile(
    r'^(Metal/|Foundation/|QuartzCore/|simd/|dispatch/|Block\.h$)')


class Tree:
    """One source tree, its skipped files and its platform units.

    A platform unit may include operating-system headers and call
    operating-system functions; its C library calls still go through lib/.
    """

    def __init__(self, root, skip, platform_units, header_exceptions,
                 identifier_exceptions):
        self.root = root
        self.skip = skip
        self.platform_units = platform_units
        self.header_exceptions = header_exceptions
        self.identifier_exceptions = identifier_exceptions


TREES = (
    Tree('renderer/src',
         skip=lambda rel: rel.startswith('shaders/') or
         rel.endswith('_data.inc'),
         platform_units=lambda rel: False,
         # The Vulkan window-system unit owns the Win32 surface types, and the
         # Metal backend programs against Metal and its Apple frameworks.
         header_exceptions=lambda rel, header: (
             rel == 'vulkan/vkr_vulkan_wsi.c' and
             header in ('windows.h', 'vulkan/vulkan_win32.h')) or (
             rel.startswith('metal/') and bool(METAL_HEADER.match(header))),
         identifier_exceptions={
             'metal/vkr_metal_packet_renderer.m': METAL_API_GCD,
             'metal/internal/vkr_metal_packet_present.inc': METAL_API_GCD,
             'metal/internal/vkr_metal_packet_setup.inc': METAL_API_GCD,
             'metal/internal/vkr_metal_packet_lifecycle.inc': METAL_API_GCD,
             'metal/internal/vkr_metal_packet_commands.inc': METAL_API_GCD,
         }),
    # runtime/src/platform/ is the runtime's window, gamepad and file-dialog
    # layer over AppKit, GameController, Win32, XInput and DXGI.
    Tree('runtime/src',
         skip=lambda rel: rel.startswith('vendor/') or
         rel == 'assets/stb_image_impl.c',
         platform_units=lambda rel: rel.startswith('platform/'),
         # Win32 wide-string APIs take wchar_t text in the Windows units.
         header_exceptions=lambda rel, header: (
             rel.startswith('platform/') and rel.endswith('_windows.c') and
             header == 'wchar.h'),
         identifier_exceptions={}),
)

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
    for tree in TREES:
        root = ROOT / tree.root
        for path in sorted(root.rglob('*')):
            if path.suffix not in SUFFIXES or not path.is_file():
                continue
            relative = path.relative_to(root).as_posix()
            if not tree.skip(relative):
                yield tree, path, relative


def header_allowed(tree, relative, header):
    if tree.header_exceptions(relative, header):
        return True
    if header in C_LIBRARY_HEADERS:
        return False
    return tree.platform_units(relative) or not OS_HEADER.match(header)


def check(tree, path, relative):
    failures = []
    shown = f'{tree.root}/{relative}'
    raw = path.read_text(encoding='utf-8')
    for number, line in enumerate(raw.splitlines(), 1):
        match = INCLUDE.match(line)
        if match and not header_allowed(tree, relative, match.group(1)):
            failures.append(f'{shown}:{number}: <{match.group(1)}> is outside '
                            'the lib boundary')
    exceptions = tree.identifier_exceptions.get(relative, set())
    platform_unit = tree.platform_units(relative)
    text = strip_comments_and_strings(raw)
    for number, line in enumerate(text.splitlines(), 1):
        if INCLUDE.match(line) or line.lstrip().startswith('#'):
            continue
        for match in IDENTIFIER.finditer(line):
            word = match.group(0)
            group = BANNED.get(word)
            if not group or word in exceptions:
                continue
            if platform_unit and group == 'platform':
                continue
            before = line[:match.start()].rstrip()
            # Members, Objective-C selectors, designated fields and C++
            # qualified names share names such as close or write with libc.
            if before.endswith(('.', '->')):
                continue
            if before.endswith('::') and not before.endswith('std::'):
                continue
            after = line[match.end():].lstrip()
            if after.startswith(':') and not after.startswith('::'):
                continue
            is_macro = word.isupper() or word.startswith(('M_', 'PRI'))
            if not is_macro and word not in ANY_USE and \
                    not after.startswith('('):
                continue
            failures.append(f'{shown}:{number}: {word} ({group}) has a lib '
                            'owner')
    return failures


def main():
    failures = []
    for tree, path, relative in sources():
        failures.extend(check(tree, path, relative))
    if failures:
        print('\n'.join(failures))
        print(f'check_libc_boundary: {len(failures)} direct libc or platform '
              'uses; use the lib API')
        return 1
    print('libc boundary guard passed (renderer, runtime)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
