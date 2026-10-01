cmake_minimum_required(VERSION 3.20)
project(AIMP_AutoRate CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Ordner mit den AIMP-SDK-Headern (apiPlugin.h, apiCore.h, ...), z.B. C:/AIMP_SDK/Cpp
set(AIMP_SDK_DIR "" CACHE PATH "Pfad zum SDK-Ordner Sources/Cpp (enthaelt apiPlugin.h)")

add_library(AIMP_AutoRate SHARED src/Plugin.cpp)
target_include_directories(AIMP_AutoRate PRIVATE ${AIMP_SDK_DIR} src)
target_compile_definitions(AIMP_AutoRate PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_link_libraries(AIMP_AutoRate PRIVATE ole32 oleaut32 uuid shell32 propsys advapi32)
set_target_properties(AIMP_AutoRate PROPERTIES OUTPUT_NAME "AIMP_AutoRate" SUFFIX ".dll")
