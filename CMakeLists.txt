cmake_minimum_required(VERSION 3.20)
project(CSMoyu LANGUAGES CXX)

add_executable(CSMoyu WIN32
    src/main.cpp
)

target_compile_features(CSMoyu PRIVATE cxx_std_17)
target_compile_definitions(CSMoyu PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(CSMoyu PRIVATE ws2_32 shell32 comctl32 comdlg32)

if(MSVC)
    target_compile_options(CSMoyu PRIVATE /W4 /permissive- /utf-8)
endif()
