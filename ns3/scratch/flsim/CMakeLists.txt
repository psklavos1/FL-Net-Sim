build_exec(
  EXECNAME silo
  SOURCE_FILES
    silo.cc
    silo/config.cc
    silo/net-builder.cc
    silo/presets.cc
  LIBRARIES_TO_LINK
    ${ns3-libs}
    ${ns3-contrib-libs}
  EXECUTABLE_DIRECTORY_PATH ${CMAKE_OUTPUT_DIRECTORY}/scratch/flsim
)

build_exec(
  EXECNAME wifi
  SOURCE_FILES
    wifi.cc
    wifi/config.cc
    wifi/net-builder.cc
    wifi/presets.cc
  LIBRARIES_TO_LINK
    ${ns3-libs}
    ${ns3-contrib-libs}
  EXECUTABLE_DIRECTORY_PATH ${CMAKE_OUTPUT_DIRECTORY}/scratch/flsim
)

build_exec(
  EXECNAME lte
  SOURCE_FILES
    lte.cc
    lte/config.cc
    lte/net-builder.cc
    lte/presets.cc
  LIBRARIES_TO_LINK
    ${ns3-libs}
    ${ns3-contrib-libs}
  EXECUTABLE_DIRECTORY_PATH ${CMAKE_OUTPUT_DIRECTORY}/scratch/flsim
)
