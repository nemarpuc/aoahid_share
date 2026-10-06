add_executable(aoahid_share_tests test_core.cpp)
target_include_directories(aoahid_share_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(aoahid_share_tests PRIVATE aoahid_share_core)
add_test(NAME aoahid_share.core COMMAND aoahid_share_tests)

# The settings window's key tables and the ini text they are edited through.
add_executable(aoahid_share_keys_tests test_keys.cpp)
target_include_directories(aoahid_share_keys_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(aoahid_share_keys_tests PRIVATE aoahid_share_core)
target_compile_definitions(aoahid_share_keys_tests PRIVATE
  AOAHID_SHARE_DOCS_DIR="${PROJECT_SOURCE_DIR}/docs")
add_test(NAME aoahid_share.keys COMMAND aoahid_share_keys_tests)

# The adb output parsers and the state file. This one links libaoahid, whose
# DLL is not next to the test on Windows, so it is built elsewhere only.
if(NOT WIN32)
  add_executable(aoahid_share_output_tests test_output.cpp)
  target_include_directories(aoahid_share_output_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(aoahid_share_output_tests PRIVATE aoahid_share_output
                                                          aoahid_share_control)
  add_test(NAME aoahid_share.output COMMAND aoahid_share_output_tests)
endif()
