cmake_minimum_required(VERSION 3.20)
project(OneTimePE VERSION 1.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(OneTimePE WIN32
    src/main.cpp
    src/net_sync.cpp
    src/app.rc
)

target_include_directories(OneTimePE PRIVATE src)
target_compile_definitions(OneTimePE PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0A00 WINVER=0x0A00)

if(MINGW)
    target_compile_options(OneTimePE PRIVATE -Wall -Wextra -O2 -municode)
    target_link_options(OneTimePE PRIVATE -municode -static -Wl,--gc-sections -s)
endif()

target_link_libraries(OneTimePE PRIVATE
    comctl32
    ole32
    shell32
    uuid
    gdi32
    user32
    iphlpapi
)

