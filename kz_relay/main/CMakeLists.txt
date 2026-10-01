idf_component_register(SRCS "kz_relay.c" "protocol.c" "journal.c" "command_engine.c" "network.c" "device_mqtt.c" "relay.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_wifi esp_netif esp_event nvs_flash esp_timer mqtt)

if(EXISTS "${PROJECT_DIR}/local/device_config.h" AND EXISTS "${PROJECT_DIR}/local/broker_ca.pem")
    target_include_directories(${COMPONENT_LIB} PRIVATE "${PROJECT_DIR}/local")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KZ_LOCAL_CONFIG=1)
    target_add_binary_data(${COMPONENT_LIB} "${PROJECT_DIR}/local/broker_ca.pem" TEXT RENAME_TO kz_broker_ca)
else()
    message(WARNING "No complete local KZ configuration: image will keep relay OFF and disable networking")
endif()
