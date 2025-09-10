# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "Debug")
  file(REMOVE_RECURSE
  "CMakeFiles\\CloudMeeting_autogen.dir\\AutogenUsed.txt"
  "CMakeFiles\\CloudMeeting_autogen.dir\\ParseCache.txt"
  "CloudMeeting_autogen"
  )
endif()
