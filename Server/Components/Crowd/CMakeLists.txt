# Crowd: the server's side of the open.mp launcher's Local crowd (omp-crowd). Players' games run
# GTA's own pedestrians and traffic; when they're together one owns the crowd around them and the
# others see copies. This component runs no AI and creates nothing in the game: it links each
# game to its player over its own UDP port (the game's + 1), relays crowd updates between players
# in range, decides owners, and lets scripts switch it on and off (omp-crowd.inc).
get_filename_component(ProjectId ${CMAKE_CURRENT_SOURCE_DIR} NAME)
add_server_component(${ProjectId})

target_compile_definitions(${ProjectId} PRIVATE
	-DPAWN_CELL_SIZE=32
	-DHAVE_STDINT_H=1
	-DLINUX
)

target_include_directories(${ProjectId} PRIVATE
	${CMAKE_SOURCE_DIR}/lib
	${CMAKE_SOURCE_DIR}/lib/pawn/source
	${CMAKE_SOURCE_DIR}/lib/pawn/source/amx
	${CMAKE_SOURCE_DIR}/lib/pawn/source/linux
)
