# Native side of the optional Plugin Bridge.
#
# Managed SDKs, adapters, DependencyControl, and their packaging are built by
# the external AegisubBridge repository. This module only supplies the native
# host implementation to Aegisub and, when explicitly requested, a smoke
# executable against a prebuilt runtime directory.

set(AEGISUB_DOTNET_HOST_PACK_NATIVE_DIR "" CACHE PATH
    "Directory containing nethost/hostfxr headers and the nethost runtime library")

if(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(_aegisub_dotnet_rid win-x86)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
        set(_aegisub_dotnet_rid win-arm64)
    else()
        set(_aegisub_dotnet_rid win-x64)
    endif()
    set(_aegisub_nethost_filename nethost.dll)
elseif(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
        set(_aegisub_dotnet_rid osx-arm64)
    else()
        set(_aegisub_dotnet_rid osx-x64)
    endif()
    set(_aegisub_nethost_filename libnethost.dylib)
else()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
        set(_aegisub_dotnet_rid linux-arm64)
    else()
        set(_aegisub_dotnet_rid linux-x64)
    endif()
    set(_aegisub_nethost_filename libnethost.so)
endif()

if(NOT AEGISUB_DOTNET_HOST_PACK_NATIVE_DIR)
    find_program(AEGISUB_DOTNET_EXECUTABLE NAMES dotnet)
    if(AEGISUB_DOTNET_EXECUTABLE)
        get_filename_component(_aegisub_dotnet_executable_realpath
            "${AEGISUB_DOTNET_EXECUTABLE}" REALPATH)
        get_filename_component(_aegisub_dotnet_root
            "${_aegisub_dotnet_executable_realpath}" DIRECTORY)
        file(GLOB _aegisub_host_pack_versions LIST_DIRECTORIES true
            "${_aegisub_dotnet_root}/packs/Microsoft.NETCore.App.Host.${_aegisub_dotnet_rid}/*")
        set(_aegisub_best_host_pack_version 0.0.0)
        foreach(_aegisub_host_pack_dir IN LISTS _aegisub_host_pack_versions)
            get_filename_component(_aegisub_host_pack_version
                "${_aegisub_host_pack_dir}" NAME)
            set(_aegisub_host_pack_native
                "${_aegisub_host_pack_dir}/runtimes/${_aegisub_dotnet_rid}/native")
            if(_aegisub_host_pack_version VERSION_GREATER _aegisub_best_host_pack_version
                AND EXISTS "${_aegisub_host_pack_native}/nethost.h"
                AND EXISTS "${_aegisub_host_pack_native}/hostfxr.h"
                AND EXISTS "${_aegisub_host_pack_native}/coreclr_delegates.h"
                AND EXISTS "${_aegisub_host_pack_native}/${_aegisub_nethost_filename}")
                set(_aegisub_best_host_pack_version "${_aegisub_host_pack_version}")
                set(AEGISUB_DOTNET_HOST_PACK_NATIVE_DIR
                    "${_aegisub_host_pack_native}")
            endif()
        endforeach()
    endif()
endif()

if(NOT AEGISUB_DOTNET_HOST_PACK_NATIVE_DIR)
    message(FATAL_ERROR
        "Could not find the ${_aegisub_dotnet_rid} native hosting pack. "
        "Set AEGISUB_DOTNET_HOST_PACK_NATIVE_DIR explicitly.")
endif()

foreach(_aegisub_host_header nethost.h hostfxr.h coreclr_delegates.h)
    if(NOT EXISTS "${AEGISUB_DOTNET_HOST_PACK_NATIVE_DIR}/${_aegisub_host_header}")
        message(FATAL_ERROR
            "Missing ${_aegisub_host_header} in ${AEGISUB_DOTNET_HOST_PACK_NATIVE_DIR}")
    endif()
endforeach()
if(NOT EXISTS "${AEGISUB_DOTNET_HOST_PACK_NATIVE_DIR}/${_aegisub_nethost_filename}")
    message(FATAL_ERROR
        "Missing ${_aegisub_nethost_filename} in ${AEGISUB_DOTNET_HOST_PACK_NATIVE_DIR}")
endif()

target_sources(Aegisub PRIVATE
    "${PROJECT_SOURCE_DIR}/src/coreclr/adapter_bridge.cpp"
    "${PROJECT_SOURCE_DIR}/src/coreclr/bridge_error.cpp"
    "${PROJECT_SOURCE_DIR}/src/coreclr/declarative_ui_host.cpp"
    "${PROJECT_SOURCE_DIR}/src/coreclr/declarative_ui_model.cpp"
    "${PROJECT_SOURCE_DIR}/src/coreclr/package_transaction_host.cpp"
    "${PROJECT_SOURCE_DIR}/src/coreclr/plugin_lua_api.cpp"
    "${PROJECT_SOURCE_DIR}/src/coreclr/plugin_service_registry.cpp"
    "${PROJECT_SOURCE_DIR}/src/coreclr/package_transaction.cpp"
    "${PROJECT_SOURCE_DIR}/src/coreclr/dotnet_automation_engine.cpp"
    "${PROJECT_SOURCE_DIR}/src/coreclr/dotnet_query_state.cpp"
    "${PROJECT_SOURCE_DIR}/src/coreclr/dotnet_subtitle_bridge.cpp"
    "${PROJECT_SOURCE_DIR}/src/coreclr/host.cpp"
    "${PROJECT_SOURCE_DIR}/src/coreclr/native_library.cpp"
    "${PROJECT_SOURCE_DIR}/src/coreclr/managed_plugin_activation.cpp"
)
target_include_directories(Aegisub PRIVATE
    "${AEGISUB_DOTNET_HOST_PACK_NATIVE_DIR}")
target_compile_definitions(Aegisub PRIVATE WITH_PLUGIN_BRIDGE=1)
target_link_libraries(Aegisub PRIVATE ${CMAKE_DL_LIBS})

# The external repository may provide a complete runtime directory for a
# native process smoke. Aegisub itself never builds or copies that directory.
set(AEGISUB_PLUGIN_BRIDGE_RUNTIME_DIR "" CACHE PATH
    "Optional prebuilt Plugin Bridge runtime root for the native smoke test")
if(AEGISUB_PLUGIN_BRIDGE_RUNTIME_DIR)
    get_filename_component(_aegisub_plugin_bridge_runtime_dir
        "${AEGISUB_PLUGIN_BRIDGE_RUNTIME_DIR}" ABSOLUTE)
    set(_aegisub_plugin_bridge_coreclr_dir
        "${_aegisub_plugin_bridge_runtime_dir}/plugins/coreclr")
    set(_aegisub_plugin_bridge_sample_dir
        "${_aegisub_plugin_bridge_runtime_dir}/automation/autoload/CSharpBridgeDemo")
    set(_aegisub_plugin_bridge_native_dir
        "${_aegisub_plugin_bridge_runtime_dir}/plugins/aegisub.dependency-control")
    foreach(_aegisub_runtime_file
        "${_aegisub_plugin_bridge_coreclr_dir}/Aegisub.CoreClr.Adapter.dll"
        "${_aegisub_plugin_bridge_coreclr_dir}/Aegisub.CoreClr.Adapter.runtimeconfig.json"
        "${_aegisub_plugin_bridge_sample_dir}/Aegisub.Managed.SampleExtension.dll"
        "${_aegisub_plugin_bridge_native_dir}/Aegisub.DependencyControl.NativeAot${CMAKE_SHARED_LIBRARY_SUFFIX}")
        if(NOT EXISTS "${_aegisub_runtime_file}")
            message(FATAL_ERROR
                "AEGISUB_PLUGIN_BRIDGE_RUNTIME_DIR is missing ${_aegisub_runtime_file}")
        endif()
    endforeach()

    add_library(aegisub-nativeaot-missing-export-fixture SHARED
        "${PROJECT_SOURCE_DIR}/tests/plugin-bridge-smoke/nativeaot_fixture.cpp")
    target_compile_features(aegisub-nativeaot-missing-export-fixture PRIVATE cxx_std_20)
    target_compile_definitions(aegisub-nativeaot-missing-export-fixture PRIVATE
        AEGISUB_NATIVEAOT_FIXTURE_MISSING_EXPORT=1)
    target_include_directories(aegisub-nativeaot-missing-export-fixture PRIVATE
        "${PROJECT_SOURCE_DIR}/src")

    add_library(aegisub-nativeaot-incompatible-fixture SHARED
        "${PROJECT_SOURCE_DIR}/tests/plugin-bridge-smoke/nativeaot_fixture.cpp")
    target_compile_features(aegisub-nativeaot-incompatible-fixture PRIVATE cxx_std_20)
    target_include_directories(aegisub-nativeaot-incompatible-fixture PRIVATE
        "${PROJECT_SOURCE_DIR}/src")

    add_executable(aegisub-plugin-bridge-smoke
        "${PROJECT_SOURCE_DIR}/src/coreclr/adapter_bridge.cpp"
        "${PROJECT_SOURCE_DIR}/src/coreclr/bridge_error.cpp"
        "${PROJECT_SOURCE_DIR}/src/coreclr/host.cpp"
        "${PROJECT_SOURCE_DIR}/src/coreclr/native_library.cpp"
        "${PROJECT_SOURCE_DIR}/src/coreclr/managed_plugin_activation.cpp"
        "${PROJECT_SOURCE_DIR}/tests/plugin-bridge-smoke/main.cpp")
    target_compile_features(aegisub-plugin-bridge-smoke PRIVATE cxx_std_20)
    target_include_directories(aegisub-plugin-bridge-smoke PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/libaegisub/include"
        "${AEGISUB_DOTNET_HOST_PACK_NATIVE_DIR}")
    target_compile_definitions(aegisub-plugin-bridge-smoke PRIVATE
        "AEGISUB_PLUGIN_BRIDGE_MANAGED_DIR=\"${_aegisub_plugin_bridge_coreclr_dir}\""
        "AEGISUB_PLUGIN_BRIDGE_SAMPLE_DIR=\"${_aegisub_plugin_bridge_sample_dir}\""
        "AEGISUB_DEPENDENCY_CONTROL_NATIVEAOT_FILENAME=\"Aegisub.DependencyControl.NativeAot${CMAKE_SHARED_LIBRARY_SUFFIX}\""
        "AEGISUB_NATIVEAOT_MISSING_EXPORT_FILENAME=\"$<TARGET_FILE_NAME:aegisub-nativeaot-missing-export-fixture>\""
        "AEGISUB_NATIVEAOT_INCOMPATIBLE_FILENAME=\"$<TARGET_FILE_NAME:aegisub-nativeaot-incompatible-fixture>\"")
    target_link_libraries(aegisub-plugin-bridge-smoke PRIVATE
        ${CMAKE_DL_LIBS} libaegisub)
    add_dependencies(aegisub-plugin-bridge-smoke
        aegisub-nativeaot-missing-export-fixture
        aegisub-nativeaot-incompatible-fixture)
    add_custom_command(TARGET aegisub-plugin-bridge-smoke POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_aegisub_plugin_bridge_coreclr_dir}/${_aegisub_nethost_filename}"
            "$<TARGET_FILE_DIR:aegisub-plugin-bridge-smoke>/${_aegisub_nethost_filename}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_aegisub_plugin_bridge_native_dir}/Aegisub.DependencyControl.NativeAot${CMAKE_SHARED_LIBRARY_SUFFIX}"
            "$<TARGET_FILE_DIR:aegisub-plugin-bridge-smoke>/Aegisub.DependencyControl.NativeAot${CMAKE_SHARED_LIBRARY_SUFFIX}"
        COMMENT "Staging the external Plugin Bridge runtime for the native smoke"
        VERBATIM)
    add_custom_target(run-aegisub-plugin-bridge-smoke
        COMMAND "$<TARGET_FILE:aegisub-plugin-bridge-smoke>"
        DEPENDS aegisub-plugin-bridge-smoke
        USES_TERMINAL
        VERBATIM)
endif()
