idf_component_register(SRC_DIRS          "."
                      PRIV_INCLUDE_DIRS  "." "${ESP_MATTER_PATH}/examples/common/utils")

if (CONFIG_ENABLE_SET_CERT_DECLARATION_API)
    target_add_binary_data(${COMPONENT_TARGET} "certification_declaration/certification_declaration.der" BINARY)
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")

# Override vendor and product names. These must match whatever is passed to
# esp-matter-mfg-tool, and CONFIG_DEVICE_VENDOR_ID/PRODUCT_ID in sdkconfig.defaults.esp32c6.
idf_build_set_property(COMPILE_DEFINITIONS "-DCHIP_DEVICE_CONFIG_DEVICE_VENDOR_NAME=\"ChaikaMatter\"" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "-DCHIP_DEVICE_CONFIG_DEVICE_PRODUCT_NAME=\"T8_7X_Proxy\"" APPEND)
