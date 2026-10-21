cmake_minimum_required(VERSION 3.15)

project(m3331_cal_tool LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(m3331_cal_tool
    src/main.c
    src/usb_hid.c
    src/cal_proto.c
)

target_include_directories(m3331_cal_tool PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# Native Windows HID access (no libusb / WinUSB / Zadig required).
target_link_libraries(m3331_cal_tool PRIVATE hid setupapi)

if(MSVC)
    target_compile_definitions(m3331_cal_tool PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(m3331_cal_tool PRIVATE /W4)
endif()
