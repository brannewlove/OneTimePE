cmake_minimum_required(VERSION 3.20)
project(OneTimePE VERSION 1.2.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_executable(OneTimePE WIN32
    src/main.cpp
    src/net_sync.cpp
    src/app.rc
)

# Always place built OneTimePE.exe directly in PEBoot/ so it never diverges from the runtime folder
set_target_properties(OneTimePE PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/PEBoot"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/PEBoot"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/PEBoot"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_SOURCE_DIR}/PEBoot"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_SOURCE_DIR}/PEBoot"
)

target_include_directories(OneTimePE PRIVATE src)
target_compile_definitions(OneTimePE PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0A00 WINVER=0x0A00)

if(MINGW)
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -c 65001")
    target_compile_options(OneTimePE PRIVATE -Wall -Wextra -O2 -municode)
    target_link_options(OneTimePE PRIVATE -municode -static -Wl,--gc-sections -s)

    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(NO_MANIFEST_SPEC "${CMAKE_BINARY_DIR}/no_default_manifest.spec")
        file(WRITE "${NO_MANIFEST_SPEC}" "*endfile:\ncrtend.o%s\n")
        target_link_options(OneTimePE PRIVATE "-specs=${NO_MANIFEST_SPEC}")
    endif()
elseif(MSVC)
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} /nologo /c65001")
    target_compile_options(OneTimePE PRIVATE /utf-8 /W4 /O2 /EHsc /MT)
    target_link_options(OneTimePE PRIVATE /MANIFEST:NO /OPT:REF /OPT:ICF)
endif()

target_link_libraries(OneTimePE PRIVATE
    comctl32
    ole32
    shell32
    uuid
    gdi32
    user32
    iphlpapi
    ws2_32
)
