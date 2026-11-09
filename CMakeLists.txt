cmake_minimum_required(VERSION 3.22)
project(DingoBench VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(DINGOBENCH_VERSION "${PROJECT_VERSION}")

find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets Svg Network)
find_package(DCMTK 3.7.1 EXACT CONFIG REQUIRED)
set(dicom_modules dcmnet dcmdata ofstd oflog)
foreach(module IN LISTS dicom_modules)
    if(NOT TARGET DCMTK::${module})
        message(FATAL_ERROR "DCMTK ${DCMTK_VERSION} is missing the DCMTK::${module} library target.")
    endif()
    list(APPEND dcmtk_targets DCMTK::${module})
endforeach()
message(STATUS "DCMTK ${DCMTK_VERSION}: ${dicom_modules}")

add_library(dingobench_infrastructure STATIC
    src/infrastructure/AppPaths.cpp
    src/infrastructure/Logger.cpp
    src/infrastructure/ProfileStore.cpp
    include/dingobench/infrastructure/Logger.h
)
set_target_properties(dingobench_infrastructure PROPERTIES AUTOMOC ON)
target_include_directories(dingobench_infrastructure PUBLIC include)
target_link_libraries(dingobench_infrastructure PUBLIC Qt6::Core PRIVATE Qt6::Network)

add_library(dingobench_dicom STATIC
    include/dingobench/dicom/OperationRunner.h
    include/dingobench/dicom/StorageReceive.h
    src/dicom/Dependencies.cpp
    src/dicom/OperationRunner.cpp
    src/dicom/UtilityOperation.cpp
    src/dicom/FindExchange.cpp
    src/dicom/MoveRetrieve.cpp
    src/dicom/GetRetrieve.cpp
    src/dicom/ReceivedObject.cpp
    src/dicom/QueryFind.cpp
    src/dicom/WorklistFind.cpp
    src/dicom/WorklistServer.cpp
    include/dingobench/dicom/WorklistServer.h
    src/dicom/Verification.cpp
    src/dicom/ClientAssociation.cpp
    src/dicom/StorageSend.cpp
    src/dicom/StorageReceive.cpp
    src/dicom/DatasetSnapshot.cpp
    src/dicom/FileInspection.cpp
    src/dicom/DiagnosticSession.cpp
)
set_target_properties(dingobench_dicom PROPERTIES AUTOMOC ON)
target_link_libraries(dingobench_dicom PUBLIC dingobench_infrastructure
    PRIVATE ${dcmtk_targets} Qt6::Network)

add_library(dingobench_ui STATIC
    src/ui/LogArchive.cpp
    src/ui/LogExplorer.cpp
    src/ui/ProtocolInspector.cpp
    src/ui/QueryRetrievePage.cpp
    src/ui/MovePage.cpp
    src/ui/GetPage.cpp
    src/ui/WorklistQueryPage.cpp
    src/ui/WorklistServerPage.cpp
    src/ui/FileInspectorPage.cpp
    src/ui/StorageSendPage.cpp
    src/ui/StorageReceivePage.cpp
    src/ui/MainWindow.cpp
    src/ui/Ui.cpp
    src/ui/WorkspacePages.cpp
)
target_include_directories(dingobench_ui PUBLIC src/ui)
target_link_libraries(dingobench_ui PUBLIC Qt6::Widgets dingobench_infrastructure PRIVATE Qt6::Svg dingobench_dicom)

qt_add_executable(DingoBench src/app/main.cpp)
if(WIN32)
    set_target_properties(DingoBench PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
add_executable(DingoBenchReceiver src/dicom/ReceiverMain.cpp)
target_link_libraries(DingoBenchReceiver PRIVATE dingobench_dicom Qt6::Network ${dcmtk_targets})
add_dependencies(DingoBench DingoBenchReceiver)
target_link_libraries(DingoBench PRIVATE dingobench_ui dingobench_dicom)
target_compile_definitions(DingoBench PRIVATE DINGOBENCH_VERSION="${DINGOBENCH_VERSION}")

set(branding_dir "${CMAKE_CURRENT_SOURCE_DIR}/assets/branding")
set(branding_resources "${branding_dir}/startup-logo.png"
    "${branding_dir}/wordmark.png" "${branding_dir}/mascot.png")
foreach(size IN ITEMS 16 24 32 48 64 128 256)
    list(APPEND branding_resources "${branding_dir}/app-icon-${size}.png")
endforeach()
set(material_dir "${CMAKE_CURRENT_SOURCE_DIR}/assets/material")
file(GLOB material_symbols CONFIGURE_DEPENDS "${material_dir}/*.svg")
function(dingobench_resources target)
    qt_add_resources(${target} ${target}_branding
        PREFIX "/branding"
        BASE "${branding_dir}"
        FILES ${branding_resources}
    )
    qt_add_resources(${target} ${target}_material
        PREFIX "/material"
        BASE "${material_dir}"
        FILES ${material_symbols} "${material_dir}/LICENSE.txt" "${material_dir}/NOTICE.txt"
    )
    qt_add_resources(${target} ${target}_workspace
        PREFIX "/workspace"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/src/ui"
        FILES src/ui/workspace.qss
    )
endfunction()
dingobench_resources(DingoBench)
install(TARGETS DingoBench DingoBenchReceiver RUNTIME DESTINATION .)
install(FILES LICENSE DESTINATION .)
install(FILES "${material_dir}/LICENSE.txt" "${material_dir}/NOTICE.txt"
        DESTINATION share/licenses/DingoBench/MaterialSymbols)

if(WIN32)
    enable_language(RC)
    target_sources(DingoBench PRIVATE "${branding_dir}/dingobench.rc")
    target_include_directories(DingoBench PRIVATE "${branding_dir}")
    set_source_files_properties("${branding_dir}/dingobench.rc" PROPERTIES
        OBJECT_DEPENDS "${branding_dir}/dingobench.ico"
    )
endif()

include(CTest)
set(DINGOBENCH_DCMTK_TOOLS_DIR "" CACHE PATH "Directory containing DCMTK loopback test utilities")
option(DINGOBENCH_REQUIRE_INTEGRATION_TOOLS "Require external DCMTK integration test utilities" OFF)
if(BUILD_TESTING)
    find_package(Qt6 6.2 REQUIRED COMPONENTS Test)

    function(dingobench_add_qt_test name target)
        add_test(NAME ${name} COMMAND ${target} ${ARGN}
            -o -,txt -o "${CMAKE_CURRENT_BINARY_DIR}/${name}-qt.txt,txt")
    endfunction()

    add_executable(dingobench_foundation_test tests/FoundationTest.cpp)
    set_target_properties(dingobench_foundation_test PROPERTIES AUTOMOC ON)
    target_link_libraries(dingobench_foundation_test PRIVATE dingobench_infrastructure Qt6::Test)
    target_compile_definitions(dingobench_foundation_test PRIVATE
        DINGOBENCH_EXECUTABLE="$<TARGET_FILE:DingoBench>"
        DINGOBENCH_VERSION="${DINGOBENCH_VERSION}"
    )
    add_dependencies(dingobench_foundation_test DingoBench)

    dingobench_add_qt_test(foundation dingobench_foundation_test)
    add_test(NAME startup_offscreen COMMAND DingoBench --smoke-test)
    add_executable(dingobench_ui_test tests/WorkspaceTest.cpp)
    set_target_properties(dingobench_ui_test PROPERTIES AUTOMOC ON)
    target_link_libraries(dingobench_ui_test PRIVATE dingobench_ui Qt6::Test Qt6::Network)
    add_dependencies(dingobench_ui_test DingoBenchReceiver)
    dingobench_resources(dingobench_ui_test)
    dingobench_add_qt_test(workspace_ui dingobench_ui_test)
    add_executable(dingobench_logging_test tests/LoggingTest.cpp)
    set_target_properties(dingobench_logging_test PROPERTIES AUTOMOC ON)
    target_link_libraries(dingobench_logging_test PRIVATE dingobench_infrastructure Qt6::Test)
    dingobench_add_qt_test(structured_logging dingobench_logging_test)
    add_executable(dingobench_profiles_test tests/ProfileStoreTest.cpp)
    set_target_properties(dingobench_profiles_test PROPERTIES AUTOMOC ON)
    target_link_libraries(dingobench_profiles_test PRIVATE dingobench_infrastructure Qt6::Test)
    dingobench_add_qt_test(connection_profiles dingobench_profiles_test)
    add_executable(dingobench_dicom_test tests/DicomInfrastructureTest.cpp)
    set_target_properties(dingobench_dicom_test PROPERTIES AUTOMOC ON)
    target_link_libraries(dingobench_dicom_test PRIVATE dingobench_dicom Qt6::Widgets Qt6::Test)
    dingobench_add_qt_test(dicom_infrastructure dingobench_dicom_test)
    add_executable(dingobench_verification_test tests/VerificationTest.cpp)
    set_target_properties(dingobench_verification_test PROPERTIES AUTOMOC ON)
    target_link_libraries(dingobench_verification_test PRIVATE dingobench_ui dingobench_dicom Qt6::Test
        Qt6::Network ${dcmtk_targets})
    dingobench_resources(dingobench_verification_test)
    add_dependencies(dingobench_verification_test DingoBench)
    dingobench_add_qt_test(dicom_verification dingobench_verification_test)
    set_property(TEST dicom_verification APPEND PROPERTY ENVIRONMENT_MODIFICATION
        "DINGOBENCH_TEST_APP=set:$<TARGET_FILE:DingoBench>")
    add_executable(dingobench_protocol_test tests/ProtocolInspectorTest.cpp)
    set_target_properties(dingobench_protocol_test PROPERTIES AUTOMOC ON)
    target_link_libraries(dingobench_protocol_test PRIVATE dingobench_ui dingobench_dicom Qt6::Test ${dcmtk_targets})
    dingobench_resources(dingobench_protocol_test)
    dingobench_add_qt_test(dicom_protocol_inspector dingobench_protocol_test)
    add_executable(dingobench_storage_test tests/StorageSendTest.cpp)
    set_target_properties(dingobench_storage_test PROPERTIES AUTOMOC ON)
    target_link_libraries(dingobench_storage_test PRIVATE dingobench_dicom Qt6::Test Qt6::Network ${dcmtk_targets})
    dingobench_add_qt_test(dicom_storage_send dingobench_storage_test)
    add_executable(dingobench_receive_test tests/StorageReceiveTest.cpp)
    set_target_properties(dingobench_receive_test PROPERTIES AUTOMOC ON)
    target_include_directories(dingobench_receive_test PRIVATE src/dicom)
    target_link_libraries(dingobench_receive_test PRIVATE dingobench_dicom Qt6::Test Qt6::Network ${dcmtk_targets})
    add_dependencies(dingobench_receive_test DingoBenchReceiver)
    dingobench_add_qt_test(dicom_storage_receive dingobench_receive_test)

    add_executable(dingobench_query_test tests/QueryFindTest.cpp)
    set_target_properties(dingobench_query_test PROPERTIES AUTOMOC ON)
    target_link_libraries(dingobench_query_test PRIVATE dingobench_ui dingobench_dicom Qt6::Test Qt6::Network ${dcmtk_targets})
    dingobench_resources(dingobench_query_test)
    dingobench_add_qt_test(dicom_query_find dingobench_query_test)

    add_executable(dingobench_worklist_test tests/WorklistFindTest.cpp)
    set_target_properties(dingobench_worklist_test PROPERTIES AUTOMOC ON)
    target_link_libraries(dingobench_worklist_test PRIVATE dingobench_ui dingobench_dicom Qt6::Test Qt6::Network ${dcmtk_targets})
    dingobench_resources(dingobench_worklist_test)
    dingobench_add_qt_test(dicom_worklist_find dingobench_worklist_test
        validatesFiltersAndHandlesRefusedConnection nativeQueriesAndLiveWorkspace nativeTimeoutAndCancellation)
    dingobench_add_qt_test(dicom_worklist_server dingobench_worklist_test
        generatesWorklistSamples guardsWorklistSampleGeneration rollsBackWorklistSampleConflict queriesGeneratedWorklist
        managedServerLifecycle managedServerStopsOnWindowClose managedServerIpv6)

    add_executable(dingobench_move_test tests/MoveRetrieveTest.cpp)
    set_target_properties(dingobench_move_test PROPERTIES AUTOMOC ON)
    target_link_libraries(dingobench_move_test PRIVATE dingobench_ui dingobench_dicom Qt6::Test Qt6::Network ${dcmtk_targets})
    add_dependencies(dingobench_move_test DingoBenchReceiver)
    dingobench_resources(dingobench_move_test)
    dingobench_add_qt_test(dicom_move_retrieve dingobench_move_test
        validatesMoveKeysAndDestination timeoutAndCancellation archiveAndLiveWorkspace)
    dingobench_add_qt_test(dicom_get_retrieve dingobench_move_test
        getNegotiationAndOutputErrors getPartialTimeoutAndCancellation getNativeArchiveAndWorkspace)

    add_executable(dingobench_file_inspection_test tests/FileInspectionTest.cpp)
    set_target_properties(dingobench_file_inspection_test PROPERTIES AUTOMOC ON)
    target_link_libraries(dingobench_file_inspection_test PRIVATE dingobench_ui dingobench_dicom Qt6::Test ${dcmtk_targets})
    dingobench_resources(dingobench_file_inspection_test)
    dingobench_add_qt_test(dicom_file_inspection dingobench_file_inspection_test)

    set(dingobench_tests foundation startup_offscreen workspace_ui structured_logging
        connection_profiles dicom_infrastructure dicom_verification dicom_protocol_inspector
        dicom_storage_send dicom_storage_receive dicom_query_find dicom_worklist_find dicom_worklist_server dicom_move_retrieve dicom_get_retrieve dicom_file_inspection)
    set_tests_properties(${dingobench_tests} PROPERTIES TIMEOUT 60)
    set(integration_tests dicom_verification dicom_storage_send dicom_storage_receive dicom_query_find
        dicom_worklist_find dicom_worklist_server dicom_move_retrieve dicom_get_retrieve)
    set_tests_properties(${integration_tests} PROPERTIES LABELS integration)
    if(DINGOBENCH_REQUIRE_INTEGRATION_TOOLS AND NOT DINGOBENCH_DCMTK_TOOLS_DIR)
        message(FATAL_ERROR "Set DINGOBENCH_DCMTK_TOOLS_DIR to the DCMTK bin directory for integration tests.")
    endif()
    if(DINGOBENCH_DCMTK_TOOLS_DIR)
        foreach(tool IN ITEMS storescp storescu dcmqrscp wlmscpfs findscu)
            set(program "${DINGOBENCH_DCMTK_TOOLS_DIR}/${tool}${CMAKE_EXECUTABLE_SUFFIX}")
            if(NOT EXISTS "${program}" OR IS_DIRECTORY "${program}")
                message(FATAL_ERROR "DCMTK integration utility missing: ${program}")
            endif()
            string(TOUPPER "${tool}" tool_upper)
            set_property(TEST ${integration_tests} APPEND PROPERTY ENVIRONMENT_MODIFICATION
                "DINGOBENCH_${tool_upper}=set:${program}")
        endforeach()
    endif()
    set_tests_properties(startup_offscreen workspace_ui dicom_infrastructure dicom_verification dicom_protocol_inspector dicom_query_find dicom_worklist_find dicom_worklist_server dicom_move_retrieve dicom_get_retrieve dicom_file_inspection
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    find_path(dingobench_iconv_data NAMES esdb/esdb.alias.db esdb/esdb.alias
        HINTS "${DCMTK_DIR}/.." "${DCMTK_DIR}/../../.."
        PATH_SUFFIXES "share/dcmtk-${DCMTK_VERSION}" "share/dcmtk" NO_CACHE)
    if(dingobench_iconv_data)
        set_property(TEST dicom_protocol_inspector dicom_query_find dicom_worklist_find dicom_worklist_server dicom_file_inspection APPEND PROPERTY ENVIRONMENT_MODIFICATION
            "DCMICONVPATH=set:${dingobench_iconv_data}")
    endif()
    if(WIN32)
        set_property(TEST ${dingobench_tests} APPEND PROPERTY ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
        foreach(library IN LISTS dcmtk_targets)
            set_property(TEST ${dingobench_tests}
                APPEND PROPERTY ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:${library}>")
        endforeach()
    endif()
endif()
