cmake_minimum_required(VERSION 3.16)
project(revlink_protocol_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Strict -std=c11 hides POSIX declarations on glibc, so strnlen() resolves as
# an implicit declaration and -Werror rejects it. ESP-IDF's newlib exposes it
# unconditionally, and so does Apple libc, which is why this only surfaces on
# Linux/GCC. Request the POSIX 2008 feature set for the host build instead of
# relaxing the standard to gnu11.
add_compile_definitions(_POSIX_C_SOURCE=200809L)

add_library(
    revlink_accessport_protocol
    ../components/accessport_protocol/revlink_accessport_protocol.c
)
target_include_directories(
    revlink_accessport_protocol
    PUBLIC ../components/accessport_protocol/include
)
target_compile_options(
    revlink_accessport_protocol
    PRIVATE -Wall -Wextra -Werror -Wpedantic
)

add_library(
    revlink_accessport_catalog
    ../components/accessport_catalog/revlink_accessport_catalog.c
)
target_include_directories(
    revlink_accessport_catalog
    PUBLIC ../components/accessport_catalog/include
)
target_compile_options(
    revlink_accessport_catalog
    PRIVATE -Wall -Wextra -Werror -Wpedantic
)

add_library(
    revlink_core
    ../components/revlink_core/revlink_device_service.c
    ../components/revlink_core/revlink_safety_policy.c
)
target_include_directories(
    revlink_core
    PUBLIC ../components/revlink_core/include
)
target_compile_options(
    revlink_core
    PRIVATE -Wall -Wextra -Werror -Wpedantic
)

add_library(
    revlink_application
    ../components/revlink_application/revlink_application.c
)
target_include_directories(
    revlink_application
    PUBLIC ../components/revlink_application/include
)
target_link_libraries(
    revlink_application
    PUBLIC revlink_accessport_protocol revlink_core revlink_sync
)
target_compile_options(
    revlink_application
    PRIVATE -Wall -Wextra -Werror -Wpedantic
)

add_library(
    revlink_sync
    ../components/revlink_sync/revlink_sync_coordinator.c
    ../components/revlink_sync/revlink_sync_manifest.c
    ../components/revlink_sync/revlink_sync_history.c
    ../components/revlink_sync/revlink_sync_annotations.c
)
target_include_directories(
    revlink_sync
    PUBLIC ../components/revlink_sync/include
)
target_compile_options(
    revlink_sync
    PRIVATE -Wall -Wextra -Werror -Wpedantic
)

add_library(
    revlink_power
    ../components/revlink_power/revlink_power_button.c
)
target_include_directories(
    revlink_power
    PUBLIC ../components/revlink_power/include
)
target_compile_options(
    revlink_power
    PRIVATE -Wall -Wextra -Werror -Wpedantic
)

add_library(
    revlink_storage_recovery
    ../components/revlink_storage_recovery/revlink_storage_recovery.c
)
target_include_directories(
    revlink_storage_recovery
    PUBLIC ../components/revlink_storage_recovery/include
)
target_compile_options(
    revlink_storage_recovery
    PRIVATE -Wall -Wextra -Werror -Wpedantic
)

add_library(
    revlink_network
    ../components/revlink_network/revlink_network_coordinator.c
)
target_include_directories(
    revlink_network
    PUBLIC ../components/revlink_network/include
)
target_compile_options(
    revlink_network
    PRIVATE -Wall -Wextra -Werror -Wpedantic
)

add_library(
    revlink_usb_control
    ../components/revlink_usb_control/revlink_usb_control.c
)
target_include_directories(
    revlink_usb_control
    PUBLIC ../components/revlink_usb_control/include
)
target_compile_options(
    revlink_usb_control
    PRIVATE -Wall -Wextra -Werror -Wpedantic
)

add_library(
    revlink_update
    ../components/revlink_update/revlink_update_health.c
)
target_include_directories(
    revlink_update
    PUBLIC ../components/revlink_update/include
)
target_compile_options(
    revlink_update
    PRIVATE -Wall -Wextra -Werror -Wpedantic
)

add_library(
    revlink_control
    ../components/revlink_control/revlink_control_service.c
)
target_include_directories(
    revlink_control
    PUBLIC ../components/revlink_control/include
)
target_link_libraries(revlink_control PUBLIC revlink_core revlink_sync)
target_compile_options(
    revlink_control
    PRIVATE -Wall -Wextra -Werror -Wpedantic
)

add_library(
    revlink_status
    ../components/revlink_status/revlink_status_model.c
)
target_include_directories(
    revlink_status
    PUBLIC ../components/revlink_status/include
)
target_link_libraries(
    revlink_status
    PUBLIC revlink_core revlink_network revlink_sync
)
target_compile_options(
    revlink_status
    PRIVATE -Wall -Wextra -Werror -Wpedantic
)

add_library(
    revlink_cli
    ../components/revlink_cli/revlink_cli.c
)
target_include_directories(
    revlink_cli
    PUBLIC ../components/revlink_cli/include
)
target_link_libraries(revlink_cli PUBLIC revlink_control)
target_compile_options(
    revlink_cli
    PRIVATE -Wall -Wextra -Werror -Wpedantic
)

add_library(
    revlink_staged_map
    ../components/revlink_staged_map/revlink_staged_map.c
)
target_include_directories(
    revlink_staged_map
    PUBLIC ../components/revlink_staged_map/include
)
target_compile_options(
    revlink_staged_map
    PRIVATE -Wall -Wextra -Werror -Wpedantic
)

add_executable(protocol_host_cli protocol_host_cli.c)
target_link_libraries(protocol_host_cli PRIVATE revlink_accessport_protocol)
target_compile_options(protocol_host_cli PRIVATE -Wall -Wextra -Werror -Wpedantic)

add_executable(delete_target_host_test delete_target_host_test.c)
target_link_libraries(
    delete_target_host_test PRIVATE revlink_accessport_protocol
)
target_compile_options(
    delete_target_host_test
    PRIVATE -Wall -Wextra -Werror -Wpedantic
)

add_executable(catalog_host_test catalog_host_test.c)
target_link_libraries(catalog_host_test PRIVATE revlink_accessport_catalog)
target_compile_options(catalog_host_test PRIVATE -Wall -Wextra -Werror -Wpedantic)

add_executable(core_host_test core_host_test.c)
target_link_libraries(core_host_test PRIVATE revlink_application)
target_compile_options(core_host_test PRIVATE -Wall -Wextra -Werror -Wpedantic)

add_executable(manifest_host_test manifest_host_test.c)
target_link_libraries(manifest_host_test PRIVATE revlink_sync)
target_compile_options(
    manifest_host_test
    PRIVATE -Wall -Wextra -Werror -Wpedantic
)

add_executable(power_host_test power_host_test.c)
target_link_libraries(power_host_test PRIVATE revlink_power)
target_compile_options(
    power_host_test
    PRIVATE -Wall -Wextra -Werror -Wpedantic
)

add_executable(
    storage_recovery_host_test
    storage_recovery_host_test.c
)
target_link_libraries(
    storage_recovery_host_test
    PRIVATE revlink_storage_recovery
)
target_compile_options(
    storage_recovery_host_test
    PRIVATE -Wall -Wextra -Werror -Wpedantic
)

add_executable(network_host_test network_host_test.c)
target_link_libraries(network_host_test PRIVATE revlink_network)
target_compile_options(
    network_host_test
    PRIVATE -Wall -Wextra -Werror -Wpedantic
)

add_library(
    revlink_captive_dns
    ../components/revlink_captive_dns/revlink_captive_dns.c
)
target_include_directories(
    revlink_captive_dns
    PUBLIC ../components/revlink_captive_dns/include
)
target_compile_options(
    revlink_captive_dns
    PRIVATE -Wall -Wextra -Werror -Wpedantic
)

add_executable(captive_dns_host_test captive_dns_host_test.c)
target_link_libraries(captive_dns_host_test PRIVATE revlink_captive_dns)
target_compile_options(
    captive_dns_host_test
    PRIVATE -Wall -Wextra -Werror -Wpedantic
)

add_library(
    revlink_credentials
    ../components/revlink_credentials/revlink_credentials.c
)
target_include_directories(
    revlink_credentials
    PUBLIC ../components/revlink_credentials/include
)
target_compile_options(
    revlink_credentials
    PRIVATE -Wall -Wextra -Werror -Wpedantic
)

add_library(
    revlink_identity
    ../components/revlink_identity/revlink_identity.c
)
target_include_directories(
    revlink_identity
    PUBLIC ../components/revlink_identity/include
)
target_compile_options(
    revlink_identity
    PRIVATE -Wall -Wextra -Werror -Wpedantic
)

add_executable(identity_host_test identity_host_test.c)
target_link_libraries(identity_host_test PRIVATE revlink_identity)
target_compile_options(
    identity_host_test
    PRIVATE -Wall -Wextra -Werror -Wpedantic
)

add_executable(credentials_host_test credentials_host_test.c)
target_link_libraries(credentials_host_test PRIVATE revlink_credentials)
target_compile_options(
    credentials_host_test
    PRIVATE -Wall -Wextra -Werror -Wpedantic
)

add_executable(usb_control_host_test usb_control_host_test.c)
target_link_libraries(usb_control_host_test PRIVATE revlink_usb_control)
target_compile_options(
    usb_control_host_test
    PRIVATE -Wall -Wextra -Werror -Wpedantic
)

add_executable(update_health_host_test update_health_host_test.c)
target_link_libraries(update_health_host_test PRIVATE revlink_update)
target_compile_options(
    update_health_host_test
    PRIVATE -Wall -Wextra -Werror -Wpedantic
)

add_executable(control_host_test control_host_test.c)
target_link_libraries(control_host_test PRIVATE revlink_control)
target_compile_options(
    control_host_test
    PRIVATE -Wall -Wextra -Werror -Wpedantic
)

add_executable(status_host_test status_host_test.c)
target_link_libraries(status_host_test PRIVATE revlink_status)
target_compile_options(
    status_host_test
    PRIVATE -Wall -Wextra -Werror -Wpedantic
)

add_executable(cli_host_test cli_host_test.c)
target_link_libraries(cli_host_test PRIVATE revlink_cli)
target_compile_options(
    cli_host_test
    PRIVATE -Wall -Wextra -Werror -Wpedantic
)

add_executable(staged_map_host_test staged_map_host_test.c)
target_link_libraries(staged_map_host_test PRIVATE revlink_staged_map)
target_compile_options(
    staged_map_host_test
    PRIVATE -Wall -Wextra -Werror -Wpedantic
)

enable_testing()
add_test(NAME protocol_unit COMMAND protocol_host_cli self-test)
add_test(NAME accessport_catalog COMMAND catalog_host_test)
add_test(NAME delete_allowlist COMMAND delete_target_host_test)
add_test(NAME core_lifecycle_and_safety COMMAND core_host_test)
add_test(NAME sync_manifest COMMAND manifest_host_test)
add_test(NAME power_button COMMAND power_host_test)
add_test(NAME storage_recovery COMMAND storage_recovery_host_test)
add_test(NAME network_coordinator COMMAND network_host_test)
add_test(NAME captive_dns_codec COMMAND captive_dns_host_test)
add_test(NAME credential_validation COMMAND credentials_host_test)
add_test(NAME sidecar_identity COMMAND identity_host_test)
add_test(NAME usb_control COMMAND usb_control_host_test)
add_test(NAME update_health_gate COMMAND update_health_host_test)
add_test(NAME control_service COMMAND control_host_test)
add_test(NAME status_model COMMAND status_host_test)
add_test(NAME cli_contract COMMAND cli_host_test)
add_test(NAME staged_map COMMAND staged_map_host_test)
# The captured-request parity test is intentionally absent from this
# repository. It replays a corpus of USB captures that is not published here.
# `protocol_unit` above still exercises the protocol builders and parsers
# against self-contained vectors.

