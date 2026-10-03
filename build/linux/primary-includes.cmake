# Prefer the pinned primary dependency headers over broad distribution include roots.
# Imported distro targets often expose all of /usr/include, which otherwise mixes
# ICU76/Boost1.83 headers with the selected ICU78/Boost1.90 static libraries.
if(AEGISUB_PRIMARY_DEPENDENCY_INCLUDE)
  set(CMAKE_NO_SYSTEM_FROM_IMPORTED ON)
  include_directories(BEFORE ${AEGISUB_PRIMARY_DEPENDENCY_INCLUDE})
endif()
