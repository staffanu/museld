find_package(PkgConfig)

pkg_check_modules(AVCODEC libavcodec REQUIRED)
pkg_check_modules(AVFORMAT libavformat REQUIRED)
pkg_check_modules(AVUTIL libavutil REQUIRED)
pkg_check_modules(SWRESAMPLE libswresample REQUIRED)
pkg_check_modules(SWSCALE libswscale REQUIRED)

find_path(AVCODEC_INCLUDE_DIR libavcodec/avcodec.h
        PATHS ${AVCODEC_INCLUDE_DIRS} /usr/include /usr/local/include /opt/local/include
        PATH_SUFFIXES libav ffmpeg
)

find_library(AVCODEC_LIBRARY avcodec
        PATHS ${AVCODEC_LIBRARY_DIRS} /usr/lib /usr/local/lib
)

find_library(AVFORMAT_LIBRARY avformat
        PATHS ${AVFORMAT_LIBRARY_DIRS} /usr/lib /usr/local/lib /opt/local/lib
)

find_library(AVUTIL_LIBRARY avutil
        PATHS ${AVUTIL_LIBRARY_DIRS} /usr/lib /usr/local/lib /opt/local/lib
)

find_library(SWRESAMPLE_LIBRARY swresample
        PATHS ${SWRESAMPLE_LIBRARY_DIRS} /usr/lib /usr/local/lib /opt/local/lib
)

find_library(SWSCALE_LIBRARY swscale
        PATHS ${SWSCALE_LIBRARY_DIRS} /usr/lib /usr/local/lib /opt/local/lib
)

set(LIBAV_INCLUDE_DIR
        ${AVCODEC_INCLUDE_DIR}
)

set(LIBAV_LIBRARIES
        ${AVCODEC_LIBRARY}
        ${AVFORMAT_LIBRARY}
        ${AVUTIL_LIBRARY}
        ${SWRESAMPLE_LIBRARY}
        ${SWSCALE_LIBRARY}
)
