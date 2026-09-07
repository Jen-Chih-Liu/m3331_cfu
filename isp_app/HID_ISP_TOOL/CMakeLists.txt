cmake_minimum_required(VERSION 3.15)

project(HID_ISP_TOOL LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(HID_ISP_TOOL
    src/main.c
    src/usb_hid.c
    src/isp_proto.c
)

target_include_directories(HID_ISP_TOOL PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# Native Windows HID access (no libusb / WinUSB / Zadig required).
target_link_libraries(HID_ISP_TOOL PRIVATE hid setupapi)

if(MSVC)
    target_compile_definitions(HID_ISP_TOOL PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(HID_ISP_TOOL PRIVATE /W4)
endif()
