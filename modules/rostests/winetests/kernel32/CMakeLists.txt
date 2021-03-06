
include_directories(BEFORE ${REACTOS_SOURCE_DIR}/sdk/include/reactos/wine)
remove_definitions(-DWINVER=0x502 -D_WIN32_IE=0x600 -D_WIN32_WINNT=0x502)
add_definitions(-DWINVER=0x600 -D_WIN32_WINNT=0x601)

list(APPEND SOURCE
    actctx.c
    atom.c
    change.c
    codepage.c
    comm.c
    console.c
    cpu.c
    debugger.c
    directory.c
    drive.c
    environ.c
    fiber.c
    file.c
    format_msg.c
    #generated.c
    heap.c
    loader.c
    locale.c
    mailslot.c
    module.c
    path.c
    pipe.c
    process.c
    profile.c
    resource.c
    sync.c
    thread.c
    time.c
    timer.c
    toolhelp.c
    version.c
    virtual.c
    volume.c
    precomp.h)

add_executable(kernel32_winetest
    ${SOURCE}
    testlist.c
    resource.rc)

if(USE_CLANG_CL OR (NOT MSVC))
    add_target_compile_flags(kernel32_winetest "-Wno-format")
endif()

set_module_type(kernel32_winetest win32cui)
add_importlibs(kernel32_winetest user32 advapi32 msvcrt kernel32 ntdll)
add_pch(kernel32_winetest precomp.h SOURCE)
add_rostests_file(TARGET kernel32_winetest)
