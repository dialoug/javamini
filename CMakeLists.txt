# CMake 构建配置 —— mini-JVM 演示项目
cmake_minimum_required(VERSION 3.15)

project(mini_jvm
    VERSION 0.1.0
    DESCRIPTION "一个简易易读的 Java 虚拟机架构演示"
    LANGUAGES CXX)

# 统一 C++ 标准:C++17,且强制要求(不静默降级)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 未指定构建类型时默认 Debug(单配置生成器,如 Makefile/Ninja 生效)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "构建类型" FORCE)
endif()

# 可执行目标
add_executable(mini_jvm mini_jvm.cpp)

# 打开常用告警(MSVC 用 /W4,其余用 -Wall -Wextra)
if(MSVC)
    target_compile_options(mini_jvm PRIVATE /W4 /utf-8)
else()
    target_compile_options(mini_jvm PRIVATE -Wall -Wextra)
endif()

# 把文本 class 文件拷到可执行文件旁边,方便默认路径加载
add_custom_command(TARGET mini_jvm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/program.mjvm $<TARGET_FILE_DIR:mini_jvm>/program.mjvm
    COMMENT "拷贝 program.mjvm 到输出目录")

# 便捷运行目标:cmake --build . --target run
add_custom_target(run
    COMMAND mini_jvm
    DEPENDS mini_jvm
    COMMENT "运行 mini-JVM")
