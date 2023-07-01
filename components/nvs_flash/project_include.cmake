# nvs_gen_partition_image
#
# Create an nvs partition image of the specified csv file on the host during build and optionally
# have the created image flashed using `idf.py flash`
function(nvs_gen_partition_image partition csv)
    set(options FLASH_IN_PROJECT)
    set(multi DEPENDS)
    cmake_parse_arguments(arg "${options}" "" "${multi}" "${ARGN}")

    idf_build_get_property(idf_path IDF_PATH)
    set(nvs_partition_gen_py ${PYTHON} ${idf_path}/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py)

    get_filename_component(csv_full_path ${csv} ABSOLUTE)

    partition_table_get_partition_info(size "--partition-name ${partition}" "size")
    partition_table_get_partition_info(offset "--partition-name ${partition}" "offset")

    if("${size}" AND "${offset}")
        set(image_file ${CMAKE_BINARY_DIR}/${partition}.bin)

        # Execute NVS partition image generation; this always executes as there is no way to specify for CMake to watch for
        # contents of the CSV changing.
        add_custom_target(nvs_${partition}_bin ALL
                COMMAND ${nvs_partition_gen_py} generate ${csv_full_path} ${image_file} ${size}
                DEPENDS ${arg_DEPENDS}
                )

        set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" APPEND PROPERTY
                ADDITIONAL_CLEAN_FILES
                ${image_file})

        idf_component_get_property(main_args esptool_py FLASH_ARGS)
        idf_component_get_property(sub_args esptool_py FLASH_SUB_ARGS)
        esptool_py_flash_target(${partition}-flash "${main_args}" "${sub_args}")
        esptool_py_flash_to_partition(${partition}-flash "${partition}" "${image_file}")

        add_dependencies(${partition}-flash nvs_${partition}_bin)

        if(arg_FLASH_IN_PROJECT)
            esptool_py_flash_to_partition(flash "${partition}" "${image_file}")
            add_dependencies(flash nvs_${partition}_bin)
        endif()
    else()
        set(message "Failed to create NVS image for partition '${partition}'. "
                "Check project configuration if using the correct partition table file.")
        fail_at_build_time(nvs_${partition}_bin "${message}")
    endif()
endfunction()