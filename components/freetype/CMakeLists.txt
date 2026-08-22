# ESP-IDF component wrapper for FreeType 2.13.2
# This replaces the upstream standalone CMakeLists.txt which uses project()
# — incompatible with ESP-IDF's component system.

idf_component_register(
    SRCS
        # Base system layer (generic, no mmap required)
        "src/base/ftsystem.c"
        "src/base/ftdebug.c"
        "src/base/ftinit.c"
        "src/base/ftbase.c"
        "src/base/ftbbox.c"
        "src/base/ftbdf.c"
        "src/base/ftbitmap.c"
        "src/base/ftcid.c"
        "src/base/ftfstype.c"
        "src/base/ftgasp.c"
        "src/base/ftglyph.c"
        "src/base/ftgxval.c"
        "src/base/ftmm.c"
        "src/base/ftotval.c"
        "src/base/ftpatent.c"
        "src/base/ftpfr.c"
        "src/base/ftstroke.c"
        "src/base/ftsynth.c"
        "src/base/fttype1.c"
        "src/base/ftwinfnt.c"
        # Font drivers and modules
        "src/autofit/autofit.c"
        "src/bdf/bdf.c"
        "src/bzip2/ftbzip2.c"
        "src/cache/ftcache.c"
        "src/cff/cff.c"
        "src/cid/type1cid.c"
        "src/gzip/ftgzip.c"
        "src/lzw/ftlzw.c"
        "src/pcf/pcf.c"
        "src/pfr/pfr.c"
        "src/psaux/psaux.c"
        "src/pshinter/pshinter.c"
        "src/psnames/psnames.c"
        "src/raster/raster.c"
        "src/sdf/sdf.c"
        "src/sfnt/sfnt.c"
        "src/smooth/smooth.c"
        "src/svg/svg.c"
        "src/truetype/truetype.c"
        "src/type1/type1.c"
        "src/type42/type42.c"
        "src/winfonts/winfnt.c"

    INCLUDE_DIRS
        "include"

    PRIV_REQUIRES
        newlib

    PRIV_INCLUDE_DIRS
        "include"
        "include/freetype"
        "include/freetype/config"
)

# FreeType needs this define when building the library itself
target_compile_definitions(${COMPONENT_LIB} PRIVATE FT2_BUILD_LIBRARY)

# On ESP32 we have unistd.h and fcntl.h — provide these as define
target_compile_definitions(${COMPONENT_LIB} PRIVATE HAVE_UNISTD_H=1 HAVE_FCNTL_H=1)

# GCC 13 false positive: storing local variable address in loader->stream,
# safe because the stream is only accessed within the function scope.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-dangling-pointer)
