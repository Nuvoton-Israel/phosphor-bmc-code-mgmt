hostver_server_hpp = custom_target(
    'server.hpp',
    capture: true,
    command: [
        sdbuspp,
        '-r', meson.source_root(),
        'interface',
        'server-header',
        'xyz.openbmc_project.Software.HostVer',
    ],
    input: '../HostVer.interface.yaml',
    install: true,
    install_dir: get_option('includedir') / 'xyz/openbmc_project/Software/HostVer',
    output: 'server.hpp',
)

hostver_server_cpp = custom_target(
    'server.cpp',
    capture: true,
    command: [
        sdbuspp,
        '-r', meson.source_root(),
        'interface',
        'server-cpp',
        'xyz.openbmc_project.Software.HostVer',
    ],
    input: '../HostVer.interface.yaml',
    output: 'server.cpp',
)