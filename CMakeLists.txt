cmake_minimum_required(VERSION 3.15)
project(MinecraftUpdater)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/MT$<$<CONFIG:Debug>:d>)
endif()

set(SOURCE_DIR ${CMAKE_SOURCE_DIR}/Source/src)
set(INCLUDE_DIR ${CMAKE_SOURCE_DIR}/Source/include)

include_directories(${INCLUDE_DIR})

find_package(PkgConfig QUIET)

if(PKG_CONFIG_FOUND)
    pkg_check_modules(CURL libcurl)
endif()

if(NOT CURL_FOUND)
    find_package(CURL REQUIRED)
endif()

find_package(OpenSSL REQUIRED)

if(PKG_CONFIG_FOUND)
    pkg_check_modules(JSONCPP jsoncpp)
endif()

if(NOT JSONCPP_FOUND)
    find_package(JsonCpp REQUIRED)
    if(JsonCpp_FOUND)
        set(JSONCPP_LIBRARIES JsonCpp::JsonCpp)
    endif()
endif()

if(PKG_CONFIG_FOUND)
    pkg_check_modules(LIBZIP libzip)
endif()

if(NOT LIBZIP_FOUND)
    find_path(LIBZIP_INCLUDE_DIR zip.h)
    find_library(LIBZIP_LIBRARY NAMES zip)
    if(LIBZIP_INCLUDE_DIR AND LIBZIP_LIBRARY)
        set(LIBZIP_FOUND TRUE)
        set(LIBZIP_INCLUDE_DIRS ${LIBZIP_INCLUDE_DIR})
        set(LIBZIP_LIBRARIES ${LIBZIP_LIBRARY})
    else()
        message(FATAL_ERROR "libzip not found")
    endif()
endif()

find_package(BZip2 REQUIRED)
if(BZIP2_FOUND)
    include_directories(${BZIP2_INCLUDE_DIRS})
endif()

include_directories(${CURL_INCLUDE_DIRS})
include_directories(${JSONCPP_INCLUDE_DIRS})
include_directories(${LIBZIP_INCLUDE_DIRS})

if(WIN32)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    add_compile_options(/W4)
    add_compile_options(/wd4996 /wd4267 /wd4244)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /NODEFAULTLIB:MSVCRT")
endif()

if(UNIX AND NOT APPLE)
    add_compile_options(-Wall -Wextra)
elseif(APPLE)
    add_compile_options(-Wall -Wextra -Wno-deprecated-declarations)
endif()

add_executable(MinecraftUpdater
    ${SOURCE_DIR}/main.cpp
    ${SOURCE_DIR}/MinecraftUpdater.cpp
    ${SOURCE_DIR}/UpdateChecker.cpp
    ${SOURCE_DIR}/ConfigManager.cpp
    ${SOURCE_DIR}/HttpClient.cpp
    ${SOURCE_DIR}/FileHasher.cpp
    ${SOURCE_DIR}/logger.cpp
)

target_link_libraries(MinecraftUpdater
    ${CURL_LIBRARIES}
    OpenSSL::SSL
    OpenSSL::Crypto
    ${JSONCPP_LIBRARIES}
    ${LIBZIP_LIBRARIES}
    ${BZIP2_LIBRARIES}
)

if(WIN32)
    target_link_libraries(MinecraftUpdater
        ws2_32
        crypt32
        advapi32
        bcrypt
        iphlpapi
        normaliz
        wldap32
    )
endif()

if(APPLE)
    find_library(CORE_FOUNDATION CoreFoundation)
    find_library(SECURITY Security)
    target_link_libraries(MinecraftUpdater 
        ${CORE_FOUNDATION} 
        ${SECURITY}
    )
endif()

file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/config)
file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/logs)

add_custom_command(TARGET MinecraftUpdater POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory 
    ${CMAKE_SOURCE_DIR}/config
    COMMAND ${CMAKE_COMMAND} -E make_directory 
    ${CMAKE_SOURCE_DIR}/logs
    COMMENT "Creating config and logs directories"
)