cmake_minimum_required(VERSION 3.19..3.19)

project(netanim CXX)
option(PACKAGING "Enable packaging build" OFF)

# Add warning regarding WSL path injection
if("$ENV{PATH}" MATCHES "/mnt/c/")
  message(
    WARNING
      "To prevent Windows path injection on WSL, append the following to /etc/wsl.conf, then use `wsl --shutdown` to restart the WSL VM:
[interop]
appendWindowsPath = false")
endif()

find_package(
  Qt6
  COMPONENTS Core Widgets PrintSupport Gui Svg CoreTools
  QUIET)
if(Qt6_FOUND)
  set(QT_VERSION_MAJOR 6)
else()
  find_package(
    Qt5
    COMPONENTS Core Widgets PrintSupport Gui
    QUIET)
  set(QT_VERSION_MAJOR 5)
endif()

if((NOT Qt6_FOUND) AND (NOT ${Qt5_FOUND}))
  message(FATAL_ERROR "You need Qt installed to build NetAnim")
endif()

# Used by qt
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(source_files
    src/animatormode.cpp
    src/animatorscene.cpp
    src/animatorview.cpp
    src/animlink.cpp
    src/animnode.cpp
    src/animpacket.cpp
    src/animpropertybrowser.cpp
    src/animresource.cpp
    src/animxmlparser.cpp
    src/countertablesscene.cpp
    src/fatal-impl.cpp
    src/flowmonstatsscene.cpp
    src/flowmonxmlparser.cpp
    src/graphpacket.cpp
    src/interfacestatsscene.cpp
    src/log.cpp
    src/logqt.cpp
    src/main.cpp
    src/mode.cpp
    src/netanim.cpp
    src/packetsmode.cpp
    src/packetsscene.cpp
    src/packetsview.cpp
    src/qcustomplot.cpp
    src/qtpropertybrowser/fileedit.cpp
    src/qtpropertybrowser/fileeditfactory.cpp
    src/qtpropertybrowser/filepathmanager.cpp
    src/qtpropertybrowser/qtbuttonpropertybrowser.cpp
    src/qtpropertybrowser/qteditorfactory.cpp
    src/qtpropertybrowser/qtgroupboxpropertybrowser.cpp
    src/qtpropertybrowser/qtpropertybrowser.cpp
    src/qtpropertybrowser/qtpropertybrowserutils.cpp
    src/qtpropertybrowser/qtpropertymanager.cpp
    src/qtpropertybrowser/qttreepropertybrowser.cpp
    src/qtpropertybrowser/qtvariantproperty.cpp
    src/resizeableitem.cpp
    src/routingstatsscene.cpp
    src/routingxmlparser.cpp
    src/statsmode.cpp
    src/statsview.cpp
    src/table.cpp
    src/textbubble.cpp)

set(src/header_files
    src/abort.h
    src/animatorconstants.h
    src/animatormode.h
    src/animatorscene.h
    src/animatorview.h
    src/animevent.h
    src/animlink.h
    src/animnode.h
    src/animpacket.h
    src/animpropertybrowser.h
    src/animresource.h
    src/animxmlparser.h
    src/assert.h
    src/common.h
    src/countertablesscene.h
    src/fatal-error.h
    src/fatal-impl.h
    src/flowmonstatsscene.h
    src/flowmonxmlparser.h
    src/graphpacket.h
    src/interfacestatsscene.h
    src/log.h
    src/logqt.h
    src/mode.h
    src/netanim.h
    src/packetsmode.h
    src/packetsscene.h
    src/packetsview.h
    src/qcustomplot.h
    src/qtpropertybrowser/QtAbstractEditorFactoryBase
    src/qtpropertybrowser/QtAbstractPropertyBrowser
    src/qtpropertybrowser/QtAbstractPropertyManager
    src/qtpropertybrowser/QtBoolPropertyManager
    src/qtpropertybrowser/QtBrowserItem
    src/qtpropertybrowser/QtButtonPropertyBrowser
    src/qtpropertybrowser/QtCharEditorFactory
    src/qtpropertybrowser/QtCharPropertyManager
    src/qtpropertybrowser/QtCheckBoxFactory
    src/qtpropertybrowser/QtColorEditorFactory
    src/qtpropertybrowser/QtColorPropertyManager
    src/qtpropertybrowser/QtCursorEditorFactory
    src/qtpropertybrowser/QtCursorPropertyManager
    src/qtpropertybrowser/QtDateEditFactory
    src/qtpropertybrowser/QtDatePropertyManager
    src/qtpropertybrowser/QtDateTimeEditFactory
    src/qtpropertybrowser/QtDateTimePropertyManager
    src/qtpropertybrowser/QtDoublePropertyManager
    src/qtpropertybrowser/QtDoubleSpinBoxFactory
    src/qtpropertybrowser/QtEnumEditorFactory
    src/qtpropertybrowser/QtEnumPropertyManager
    src/qtpropertybrowser/QtFlagPropertyManager
    src/qtpropertybrowser/QtFontEditorFactory
    src/qtpropertybrowser/QtFontPropertyManager
    src/qtpropertybrowser/QtGroupBoxPropertyBrowser
    src/qtpropertybrowser/QtGroupPropertyManager
    src/qtpropertybrowser/QtIntPropertyManager
    src/qtpropertybrowser/QtKeySequenceEditorFactory
    src/qtpropertybrowser/QtKeySequencePropertyManager
    src/qtpropertybrowser/QtLineEditFactory
    src/qtpropertybrowser/QtLocalePropertyManager
    src/qtpropertybrowser/QtPointFPropertyManager
    src/qtpropertybrowser/QtPointPropertyManager
    src/qtpropertybrowser/QtProperty
    src/qtpropertybrowser/QtRectFPropertyManager
    src/qtpropertybrowser/QtRectPropertyManager
    src/qtpropertybrowser/QtScrollBarFactory
    src/qtpropertybrowser/QtSizeFPropertyManager
    src/qtpropertybrowser/QtSizePolicyPropertyManager
    src/qtpropertybrowser/QtSizePropertyManager
    src/qtpropertybrowser/QtSliderFactory
    src/qtpropertybrowser/QtSpinBoxFactory
    src/qtpropertybrowser/QtStringPropertyManager
    src/qtpropertybrowser/QtTimeEditFactory
    src/qtpropertybrowser/QtTimePropertyManager
    src/qtpropertybrowser/QtTreePropertyBrowser
    src/qtpropertybrowser/QtVariantEditorFactory
    src/qtpropertybrowser/QtVariantProperty
    src/qtpropertybrowser/QtVariantPropertyManager
    src/qtpropertybrowser/fileedit.h
    src/qtpropertybrowser/fileeditfactory.h
    src/qtpropertybrowser/filepathmanager.h
    src/qtpropertybrowser/qtbuttonpropertybrowser.h
    src/qtpropertybrowser/qteditorfactory.h
    src/qtpropertybrowser/qtgroupboxpropertybrowser.h
    src/qtpropertybrowser/qtpropertybrowser.h
    src/qtpropertybrowser/qtpropertybrowserutils_p.h
    src/qtpropertybrowser/qtpropertymanager.h
    src/qtpropertybrowser/qttreepropertybrowser.h
    src/qtpropertybrowser/qtvariantproperty.h
    src/resizeableitem.h
    src/routingstatsscene.h
    src/routingxmlparser.h
    src/statisticsconstants.h
    src/statsmode.h
    src/statsview.h
    src/table.h
    src/textbubble.h
    src/timevalue.h)

set(resource_files resources/resources.qrc
                   src/qtpropertybrowser/qtpropertybrowser.qrc)

add_compile_definitions(NS3_LOG_ENABLE)

if(PACKAGING)
  qt_add_executable(netanim MANUAL_FINALIZATION ${source_files} ${header_files}
                    ${resource_files})
else()
  add_executable(netanim ${source_files} ${header_files} ${resource_files})
endif()

if(Qt6_FOUND)
  target_link_libraries(netanim PUBLIC Qt::Widgets Qt::Core Qt::PrintSupport
                                       Qt::Gui Qt::Svg)
else()
  target_link_libraries(netanim PUBLIC Qt5::Widgets Qt5::Core Qt5::PrintSupport
                                       Qt5::Gui)
endif()

target_include_directories(netanim PRIVATE src/qtpropertybrowser)

set_target_properties(
  netanim
  PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE
             INSTALL_RPATH "$ORIGIN:$ORIGIN/lib:$ORIGIN/lib/x86_64-linux-gnu"
             INSTALL_RPATH_USE_LINK_PATH TRUE)

if(PACKAGING)
  qt_finalize_target(netanim)
endif()

include(GNUInstallDirs)
install(TARGETS netanim RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(UNIX AND NOT APPLE)
  install(FILES data/org.nsnam.NetAnim.metainfo.xml DESTINATION share/metainfo)

  install(FILES data/org.nsnam.NetAnim.desktop DESTINATION share/applications)

  install(FILES data/org.nsnam.NetAnim.png
          DESTINATION share/icons/hicolor/128x128/apps)
endif()

if(PACKAGING)
  include(InstallRequiredSystemLibraries)

  qt_generate_deploy_app_script(TARGET netanim OUTPUT_SCRIPT deploy_script
                                NO_UNSUPPORTED_PLATFORM_ERROR)
  install(SCRIPT ${deploy_script})

  # Setup CPack for packaging your app including Qt dependencies
  set(CPACK_PACKAGE_NAME "Netanim")
  set(CPACK_PACKAGE_VERSION "3.110")
  set(CPACK_PACKAGE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/packages")

  if(WIN32)
    set(CPACK_GENERATOR "ZIP")
  elseif(MACOS)
    set(CPACK_GENERATOR "BUNDLE")
  else()
    set(CPACK_GENERATOR "ZIP") # AppImage support will land on cmake 4.2
  endif()

  # Provide license file
  set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")

  include(CPack)

  # Generate CPack configuration
  cpack_add_component_group(Runtime)
  cpack_add_component(
    runtime
    DISPLAY_NAME "Qt Runtime Libraries and Plugins"
    DESCRIPTION "Qt runtime dependencies bundled with the application"
    GROUP Runtime
    REQUIRED
    INSTALL_TYPES Full)
endif()
