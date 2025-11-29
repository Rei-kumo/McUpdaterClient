cmake_minimum_required(VERSION 3.15)
project(MinecraftUpdater)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# 设置源码和头文件路径
set(SOURCE_DIR ${CMAKE_SOURCE_DIR}/Source/src)
set(INCLUDE_DIR ${CMAKE_SOURCE_DIR}/Source/include)

# 包含目录
include_directories(${INCLUDE_DIR})

# Windows特定设置
if(WIN32)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    add_compile_options(/W4)
    # 禁用特定警告
    add_compile_options(/wd4996 /wd4267 /wd4244)
endif()

# 添加可执行文件
add_executable(MinecraftUpdater
    ${SOURCE_DIR}/main.cpp
    ${SOURCE_DIR}/MinecraftUpdater.cpp
    ${SOURCE_DIR}/UpdateChecker.cpp
    ${SOURCE_DIR}/ConfigManager.cpp
    ${SOURCE_DIR}/HttpClient.cpp
    ${SOURCE_DIR}/FileHasher.cpp
    ${SOURCE_DIR}/logger.cpp
)

# Windows链接库
if(WIN32)
    target_link_libraries(MinecraftUpdater
        ws2_32
        crypt32
        advapi32
    )
endif()

# 创建必要的目录
file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/config)
file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/logs)