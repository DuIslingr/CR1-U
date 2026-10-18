# macOS: Vulkan through MoltenVK (the LunarG SDK's: universal) and MetalFX's upscaler beside FSR.
# cr_apple_vulkan(target): the Metal frameworks (MetalFX weakly: macOS 13+, asked at run time),
# and a .app carrying the Vulkan loader and MoltenVK (Contents/Frameworks) with MoltenVK's ICD
# manifest in Contents/Resources/vulkan/icd.d, where the loader looks inside a bundle - so the
# .app needs no Vulkan SDK where it runs.
function(cr_apple_vulkan target)
    target_link_libraries(${target} PRIVATE "-framework Metal" "-framework Foundation" "-weak_framework MetalFX")
    get_target_property(bundle ${target} MACOSX_BUNDLE)
    if(NOT bundle)
        return()
    endif()
    file(REAL_PATH "${Vulkan_LIBRARY}" loader)
    get_filename_component(libdir "${Vulkan_LIBRARY}" DIRECTORY)
    if(NOT EXISTS "${libdir}/libMoltenVK.dylib")
        message(WARNING "MoltenVK not beside ${Vulkan_LIBRARY}: the .app will need a Vulkan SDK installed")
        return()
    endif()
    file(REAL_PATH "${libdir}/libMoltenVK.dylib" moltenvk)
    set_target_properties(${target} PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "@executable_path/../Frameworks")
    set(icd "${CMAKE_CURRENT_BINARY_DIR}/MoltenVK_icd.json")
    file(WRITE ${icd} "{\n    \"file_format_version\": \"1.0.0\",\n    \"ICD\": {\n"
                      "        \"library_path\": \"../../../Frameworks/libMoltenVK.dylib\",\n"
                      "        \"api_version\": \"1.2.0\",\n        \"is_portability_driver\": true\n    }\n}\n")
    set(contents $<TARGET_BUNDLE_CONTENT_DIR:${target}>)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${contents}/Frameworks ${contents}/Resources/vulkan/icd.d
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${loader} ${contents}/Frameworks/libvulkan.1.dylib
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${moltenvk} ${contents}/Frameworks/libMoltenVK.dylib
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${icd} ${contents}/Resources/vulkan/icd.d/MoltenVK_icd.json
        COMMENT "MoltenVK into the .app")
    message(STATUS "Upscalers (macOS): MetalFX (macOS 13+); MoltenVK bundled from ${libdir}")
endfunction()
