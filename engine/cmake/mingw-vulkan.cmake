# Cross-building for Windows with llvm-mingw, which has no Vulkan SDK: the host's Vulkan headers,
# and an import library for vulkan-1.dll (Windows' loader, or Wine's) made from vulkan-1.def -
# the functions the engine calls. Sets Vulkan_FOUND and Vulkan::Vulkan as FindVulkan would.
find_path(CR_MINGW_VULKAN_INCLUDE vulkan/vulkan.h PATHS /usr/include /usr/local/include NO_CMAKE_FIND_ROOT_PATH NO_DEFAULT_PATH)
find_program(CR_DLLTOOL NAMES ${LLVM_MINGW}/bin/llvm-dlltool llvm-dlltool x86_64-w64-mingw32-dlltool)
if(CR_MINGW_VULKAN_INCLUDE AND CR_DLLTOOL)
    set(lib ${CMAKE_CURRENT_BINARY_DIR}/libvulkan-1.a)
    execute_process(COMMAND ${CR_DLLTOOL} -m i386:x86-64 -d ${CMAKE_CURRENT_LIST_DIR}/vulkan-1.def -l ${lib}
                    RESULT_VARIABLE rc)
    if(rc EQUAL 0)
        add_library(Vulkan::Vulkan UNKNOWN IMPORTED)
        set_target_properties(Vulkan::Vulkan PROPERTIES IMPORTED_LOCATION ${lib})
        # (the headers alone: not the host's whole /usr/include)
        file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/vulkan-headers)
        file(COPY ${CR_MINGW_VULKAN_INCLUDE}/vulkan ${CR_MINGW_VULKAN_INCLUDE}/vk_video
             DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/vulkan-headers)
        set_target_properties(Vulkan::Vulkan PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_CURRENT_BINARY_DIR}/vulkan-headers)
        set(Vulkan_FOUND TRUE)
        set(Vulkan_VERSION "(host headers, llvm-mingw import library)")
    endif()
endif()
