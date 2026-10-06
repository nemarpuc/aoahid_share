add_executable(aoahid_share_daemon aoahid_share_daemon/main.cpp)
target_link_libraries(aoahid_share_daemon PRIVATE aoahid_share_daemon_lib)
target_compile_definitions(aoahid_share_daemon PRIVATE
  AOAHID_SHARE_VERSION="${PROJECT_VERSION}")
aoahid_share_warnings(aoahid_share_daemon)
if(WIN32)
  # No console window: the daemon runs in the background.
  set_target_properties(aoahid_share_daemon PROPERTIES WIN32_EXECUTABLE ON)
  if(MSVC)
    target_link_options(aoahid_share_daemon PRIVATE /ENTRY:mainCRTStartup)
  else()
    target_link_options(aoahid_share_daemon PRIVATE -mwindows)
  endif()
endif()

add_executable(aoahid_share aoahid_share/main.cpp)
target_link_libraries(aoahid_share PRIVATE aoahid_share_control)
target_compile_definitions(aoahid_share PRIVATE AOAHID_SHARE_VERSION="${PROJECT_VERSION}")
aoahid_share_warnings(aoahid_share)

install(TARGETS aoahid_share_daemon aoahid_share RUNTIME DESTINATION bin)

if(AOAHID_SHARE_BUILD_GUI)
  add_subdirectory(aoahid_share_gui)
endif()
