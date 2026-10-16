# h2net: static library + mesh test.
#   cmake -S net -B <build> -G Ninja -DCMAKE_BUILD_TYPE=Release ^
#     -DCMAKE_TOOLCHAIN_FILE=D:/n64work/vcpkg/scripts/buildsystems/vcpkg.cmake ^
#     -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake_minimum_required(VERSION 3.21)
project(h2net C CXX)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # /MT, like the game

find_package(LibDataChannel CONFIG REQUIRED)

add_library(h2net STATIC h2net.c h2net.h)
target_include_directories(h2net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(TARGET LibDataChannel::LibDataChannelStatic)
  target_link_libraries(h2net PUBLIC LibDataChannel::LibDataChannelStatic)
else()
  target_link_libraries(h2net PUBLIC LibDataChannel::LibDataChannel)
endif()

add_executable(h2net_test test/h2net_test.c)
target_link_libraries(h2net_test PRIVATE h2net winmm)

install(TARGETS h2net ARCHIVE DESTINATION lib)
install(FILES h2net.h DESTINATION include)
