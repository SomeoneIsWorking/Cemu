add_library(CemuBoot
  SystemBringup.cpp
  SystemBringup.h
)

set_property(TARGET CemuBoot PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

target_include_directories(CemuBoot PUBLIC "../")

target_link_libraries(CemuBoot PRIVATE
  CemuAudio
  CemuCafe
  CemuCommon
  CemuComponents
  CemuConfig
  CemuInput
  CemuUtil
)

cemu_use_precompiled_header(CemuBoot)
