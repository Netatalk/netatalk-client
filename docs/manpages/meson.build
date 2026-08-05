manfiles = [
    'afpgetstatus.1',
    'afpsld.1',
]

if with_afpcmd
    manfiles += ['afpcmd.1']
endif

if with_fuse
    manfiles += [
        'afpc.1',
        'afpfsd.1',
        'mount_afpfs.1',
    ]
endif

install_man(manfiles)
