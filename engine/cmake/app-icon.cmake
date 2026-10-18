# The port's own icon (packaging/: two rings, not the game's art - packaging/make_icons.py makes
# the .ico and .icns from packaging/linux/<id>.svg): the .exe's (a resource script, MSVC's rc or
# MinGW's windres) and the .app's (Contents/Resources, CFBundleIconFile).
function(cr_app_icon target id)
    set(packaging ${CMAKE_CURRENT_SOURCE_DIR}/../packaging)
    if(WIN32 AND EXISTS ${packaging}/windows/${id}.ico)
        set(rc ${CMAKE_CURRENT_BINARY_DIR}/${id}_icon.rc)
        file(WRITE ${rc} "1 ICON \"${packaging}/windows/${id}.ico\"\n")
        target_sources(${target} PRIVATE ${rc})
    elseif(APPLE AND EXISTS ${packaging}/macos/${id}.icns)
        set(icns ${packaging}/macos/${id}.icns)
        set_source_files_properties(${icns} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        target_sources(${target} PRIVATE ${icns})
        set_target_properties(${target} PROPERTIES MACOSX_BUNDLE_ICON_FILE ${id}.icns)
    endif()
endfunction()
