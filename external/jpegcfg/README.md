# libjpeg-turbo build configuration

    jconfig.h      what upstream's CMake would generate for this toolchain
    jconfigint.h   internal build configuration
    jversion.h     version strings

libjpeg-turbo generates these three from `.in` templates at configure time.
castlemist compiles the library's sources directly (see `cmake/Externals.cmake`)
rather than running its build system, so it supplies the trio by hand and puts
this directory **first** on libjpeg-turbo's include path.

Tracked in git, unlike the rest of `external/`: they are castlemist's own files,
not a third-party checkout. Regenerate them only if `external/libjpeg-turbo-*`
is bumped to a version whose configuration differs.
