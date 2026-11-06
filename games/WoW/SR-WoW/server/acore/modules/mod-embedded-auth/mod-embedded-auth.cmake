# included by modules/CMakeLists.txt after the `modules` target exists: where the authserver's headers are
target_include_directories(modules
  PRIVATE
    ${CMAKE_SOURCE_DIR}/src/server/apps/authserver
    ${CMAKE_SOURCE_DIR}/src/server/apps/authserver/Authentication
    ${CMAKE_SOURCE_DIR}/src/server/apps/authserver/Server)
