#
# Redistribution and use is allowed according to the terms of the BSD license.
# For details see the accompanying COPYING-CMAKE-SCRIPTS file.

find_package(Qt${QT_VERSION_MAJOR} REQUIRED NO_MODULE COMPONENTS Test)

add_executable(test_imageprocessing ${CMAKE_CURRENT_SOURCE_DIR}/test_imageprocessing.cpp)

target_link_libraries(test_imageprocessing

                      imagemosaicwallcore

                      Qt${QT_VERSION_MAJOR}::Core
                      Qt${QT_VERSION_MAJOR}::Gui
                      Qt${QT_VERSION_MAJOR}::Widgets
                      Qt${QT_VERSION_MAJOR}::Concurrent
                      Qt${QT_VERSION_MAJOR}::Test

                      ${CMAKE_THREAD_LIBS_INIT}
)

add_test(NAME test_imageprocessing COMMAND test_imageprocessing)
set_tests_properties(test_imageprocessing PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
