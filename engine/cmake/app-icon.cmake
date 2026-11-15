# The port's own icon (packaging/: two rings, not the game's art - packaging/make_icons.py makes
# the .ico and .icns from packaging/linux/<id>.svg): the .exe's (a resource script, MSVC's rc or
# MinGW's windres), the .app's (Contents/Resources, CFBundleIconFile) and the iOS app's (an asset catalog).
#
# On Windows the .exe also carries packaging/windows/utf8.manifest: UTF-8 as the process's ANSI code
# page (Windows 10 1903 on; older ones ignore it), so the narrow file calls - fopen, ifstream,
# std::filesystem's strings, FreeType's and libarchive's opens - take the UTF-8 paths SDL hands out.
# Without it a folder or user name outside the system's code page (C:\Users\Иван on a Russian
# Windows, any Cyrillic name on a Western one) reads as another path: settings, saves and copies fail.
# MSVC's linker merges a .manifest source into its own; MinGW takes it as resource 1, type 24.
function(cr_app_icon target id)
    set(packaging ${CMAKE_CURRENT_SOURCE_DIR}/../packaging)
    if(WIN32)
        set(rcText "")
        if(EXISTS ${packaging}/windows/${id}.ico)
            string(APPEND rcText "1 ICON \"${packaging}/windows/${id}.ico\"\n")
        endif()
        if(EXISTS ${packaging}/windows/utf8.manifest)
            if(MSVC)
                target_sources(${target} PRIVATE ${packaging}/windows/utf8.manifest)
            else()
                string(APPEND rcText "1 24 \"${packaging}/windows/utf8.manifest\"\n")
            endif()
        endif()
        if(rcText)
            set(rc ${CMAKE_CURRENT_BINARY_DIR}/${id}_icon.rc)
            file(WRITE ${rc} "${rcText}")
            target_sources(${target} PRIVATE ${rc})
        endif()
    elseif(IOS AND EXISTS ${packaging}/ios/Assets.xcassets)
        # iOS: the asset catalog's AppIcon (packaging/make_icons.py: one opaque 1024 x 1024), compiled by
        # Xcode's actool into the app's Assets.car; Info.plist names it (CFBundleIcons)
        set(assets ${packaging}/ios/Assets.xcassets)
        set_source_files_properties(${assets} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        target_sources(${target} PRIVATE ${assets})
        set_target_properties(${target} PROPERTIES XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME AppIcon)
    elseif(APPLE AND EXISTS ${packaging}/macos/${id}.icns)
        set(icns ${packaging}/macos/${id}.icns)
        set_source_files_properties(${icns} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        target_sources(${target} PRIVATE ${icns})
        set_target_properties(${target} PROPERTIES MACOSX_BUNDLE_ICON_FILE ${id}.icns)
    endif()
endfunction()
