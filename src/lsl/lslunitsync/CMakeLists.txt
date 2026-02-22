option(ADD_WXCONVERT "add wx convert functions to image lib" OFF)

set(libUnitsyncSrc
	"${CMAKE_CURRENT_SOURCE_DIR}/c_api.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/sharedlib.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/image.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/loader.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/mmoptionmodel.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/optionswrapper.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/unitsync.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/unitsync_cache.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/springbundle.cpp"
)
file(GLOB RECURSE libUnitsyncHeader "${CMAKE_CURRENT_SOURCE_DIR}/*.h")

#grep -lR cc\"$ "${CMAKE_CURRENT_SOURCE_DIR}/* | sed -e "s/\.h/\.cc/g"
set(templatesources "")
list(APPEND libUnitsyncHeader ${templatesources})
set_source_files_properties(${libUnitsyncHeader} PROPERTIES HEADER_FILE_ONLY 1)

#cimage deps
find_package(PNG REQUIRED)
find_package(X11 REQUIRED)
add_library(lsl-unitsync STATIC ${libUnitsyncHeader} ${libUnitsyncSrc} ${jsonlibcppsrc})
if(ADD_WXCONVERT)
	target_compile_definitions(lsl-unitsync PRIVATE -DHAVE_WX)
endif()

if(PRD_JSONCPP_INTERNAL)
	# Suppress deprecation warnings coming from bundled jsoncpp legacy API
	# implementation code (third-party).
	set_source_files_properties(
		${pr-downloader_SOURCE_DIR}/src/lib/jsoncpp/src/lib_json/json_reader.cpp
		PROPERTIES COMPILE_FLAGS "-Wno-deprecated-declarations"
	)
endif()

if(UNIX AND NOT MINGW AND NOT APPLE)
	find_library(RT_LIBRARY rt)
endif()

find_package(Threads REQUIRED)
if(CMAKE_USE_PTHREADS_INIT)
	target_link_libraries(lsl-unitsync pthread)
endif()

target_link_libraries(lsl-unitsync lsl-utils ${Boost_THREAD_LIBRARY} ${Boost_SYSTEM_LIBRARY} ${PNG_LIBRARY} ${X11_LIBRARIES} ${CMAKE_DL_LIBS} ${RT_LIBRARY} ${PRD_JSONCPP_LIBRARIES})
target_include_directories(lsl-unitsync
	PRIVATE
		${pr-downloader_SOURCE_DIR}/src
		${pr-downloader_SOURCE_DIR}/src/lsl
		${PRD_JSONCPP_INCLUDE_DIR}
)

# Third-party headers live under src/lib (e.g. CImg) and are treated as system
# headers to avoid warning spam from external code.
target_include_directories(lsl-unitsync SYSTEM PRIVATE
	${pr-downloader_SOURCE_DIR}/src/lib
)
