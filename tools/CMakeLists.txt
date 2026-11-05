# Every tools/<name>.cpp becomes its own <name>.exe. Tools may use common, host_a and client_b code
# (include "window_picker.h" etc. directly). Static libraries only pull in the objects a tool needs.
file(GLOB TOOL_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

foreach(tool_source IN LISTS TOOL_SOURCES)
  get_filename_component(tool_name ${tool_source} NAME_WE)
  add_executable(${tool_name} ${tool_source})
  target_link_libraries(${tool_name} PRIVATE kvm_host_lib kvm_client_lib)
endforeach()

# --- Tests (ctest) ------------------------------------------------------------------------------
# Every tools/*_test.cpp is listed in exactly one group below; configure fails otherwise, so a new
# test is never silently left out. The label says what a test needs:
#   unit     nothing but its own process and 127.0.0.1. Runs in CI.
#   desktop  an unlocked interactive session: hooks, SendInput, the clipboard, OLE drag and drop.
#            Hands off the mouse and keyboard while they run.
#   vdd      desktop + the Virtual Display Driver installed. Some share a Paint window that the
#            paint_window fixture opens and closes (test_window.ps1).
#   loopback the real kvm_host + kvm_client on 127.0.0.1 (free ports, no hooks, no discovery). Runs in CI.
#   gpu      a GPU with a D3D11 video processor / Media Foundation hardware codecs. CI runners have
#            none, so only ctest --preset local runs them.
#   bench    prints timings and passes unless it fails to run (jpeg_test).
#   manual   hands-on tools, not registered: run them by hand.
# Presets: `ctest --preset ci` (unit + loopback) and `ctest --preset local` (+ desktop + vdd, at the PC).
set(KVM_UNIT_TESTS
    protocol_test queue_test layout_sync_test display_layout_test launcher_test dpi_test video_test
    video_sender_test file_transfer_test net_listen_test tls_test identity_test paired_test discovery_test
    preauth_test video_stats_test annexb_test h264_decode_test viewer_h264_test codec_test bitrate_test
    audio_codec_test)
set(KVM_DESKTOP_TESTS
    win32_input_test input_capture_test input_inject_test kvm_link_test clipboard_test file_drop_test
    file_drag_test)
set(KVM_VDD_TESTS
    vdd_test window_mover_test window_share_test capture_test desk_test desk_hook_test remote_input_test)
set(KVM_LOOPBACK_TESTS loopback_test wire_test)
set(KVM_GPU_TESTS nv12_test h264_test)
set(KVM_BENCH_TESTS jpeg_test)
set(KVM_MANUAL_TESTS kvm_local_test viewer_test network_category_test)

# tls_test drives plain OpenSSL clients too (TLS 1.2, no certificate) to check what is refused.
target_link_libraries(tls_test PRIVATE OpenSSL::SSL OpenSSL::Crypto)
# audio_codec_test measures in-band FEC on raw Opus too.
target_link_libraries(audio_codec_test PRIVATE Opus::opus)

# Arguments for tests that would otherwise ask which window to use.
set(KVM_PAINT_TESTS window_mover_test window_share_test capture_test desk_test remote_input_test)
set(window_mover_test_ARGS --title Paint)
set(window_share_test_ARGS --title Paint)
set(capture_test_ARGS --title Paint)
# --settle: Windows re-places windows for a few seconds after the VDD is attached.
set(desk_test_ARGS --auto --title Paint --settle 3)
set(remote_input_test_ARGS --title Paint)
# A stream Sarke's encoder made (h264_test --write-sample), decoded in software.
set(h264_decode_test_ARGS ${CMAKE_CURRENT_SOURCE_DIR}/data/h264_sample.h264)
set(viewer_h264_test_ARGS ${CMAKE_CURRENT_SOURCE_DIR}/data/h264_sample.h264)

set(kvm_known_tests ${KVM_UNIT_TESTS} ${KVM_DESKTOP_TESTS} ${KVM_VDD_TESTS} ${KVM_LOOPBACK_TESTS} ${KVM_GPU_TESTS}
    ${KVM_BENCH_TESTS}
    ${KVM_MANUAL_TESTS})
foreach(tool_source IN LISTS TOOL_SOURCES)
  get_filename_component(tool_name ${tool_source} NAME_WE)
  if(tool_name MATCHES "_test$" AND NOT tool_name IN_LIST kvm_known_tests)
    message(FATAL_ERROR "tools/${tool_name}.cpp: add it to one of the test groups in tools/CMakeLists.txt")
  endif()
endforeach()

function(kvm_add_tests label timeout)
  foreach(test_name IN LISTS ARGN)
    add_test(NAME ${test_name} COMMAND ${test_name} ${${test_name}_ARGS})
    set_tests_properties(${test_name} PROPERTIES LABELS ${label} TIMEOUT ${timeout})
  endforeach()
endfunction()

kvm_add_tests(unit 120 ${KVM_UNIT_TESTS})
kvm_add_tests(desktop 120 ${KVM_DESKTOP_TESTS})
kvm_add_tests(vdd 300 ${KVM_VDD_TESTS})
kvm_add_tests(loopback 120 ${KVM_LOOPBACK_TESTS})
kvm_add_tests(gpu 120 ${KVM_GPU_TESTS})
kvm_add_tests(bench 300 ${KVM_BENCH_TESTS})

# One mouse, one keyboard, one clipboard and one VDD: these never run in parallel with each other.
set_tests_properties(${KVM_DESKTOP_TESTS} PROPERTIES RESOURCE_LOCK desktop)
set_tests_properties(${KVM_VDD_TESTS} PROPERTIES RESOURCE_LOCK "desktop;vdd")

# A Paint window for the tests that share one; only the Paint this fixture started is closed.
set(paint_pid_file ${CMAKE_CURRENT_BINARY_DIR}/paint_window.pid)
add_test(NAME paint_window_open
         COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_CURRENT_SOURCE_DIR}/test_window.ps1
                 -Start mspaint -PidFile ${paint_pid_file})
add_test(NAME paint_window_close
         COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_CURRENT_SOURCE_DIR}/test_window.ps1
                 -Stop -PidFile ${paint_pid_file})
set_tests_properties(paint_window_open PROPERTIES FIXTURES_SETUP paint_window LABELS vdd TIMEOUT 60)
set_tests_properties(paint_window_close PROPERTIES FIXTURES_CLEANUP paint_window LABELS vdd TIMEOUT 60)
set_tests_properties(${KVM_PAINT_TESTS} PROPERTIES FIXTURES_REQUIRED paint_window)
