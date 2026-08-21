cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(metalio_radio)

include(elf_loader)

# The firmware loader allocates the complete PT_LOAD virtual-address span in
# internal executable RAM, so avoid the GNU ld default 4 KiB page gaps.
set(ELF_LIBS "-Wl,-z,max-page-size=16")
project_elf(metalio_radio)
