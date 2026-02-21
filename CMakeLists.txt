cmake_minimum_required(VERSION 3.20)
project(GridNotesCpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(GridNotes WIN32
    src/main.cpp
)

target_compile_definitions(GridNotes PRIVATE UNICODE _UNICODE)

target_link_libraries(GridNotes PRIVATE
    user32
    gdi32
    comctl32
    shell32
    advapi32
)
