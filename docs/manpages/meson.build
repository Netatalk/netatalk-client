manfiles = [
    'afpgetstatus.1',
    'afpsld.1',
]

markdown_manfiles = [
    'afpc.1',
    'afpcmd.1',
    'afpfsd.1',
    'afpgetstatus.1',
    'afpsld.1',
    'mount_afpfs.1',
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

# mandoc understands mdoc natively and produces Markdown without treating the
# source as generic roff.  The wrapper removes terminal-reader chrome and
# gives each generated page a website-friendly title.
mandoc = find_program('mandoc', required: website_docs)

if website_docs and mandoc.found()
    sh = find_program('sh', required: true)
    mdoc_to_markdown = files('mdoc_to_markdown.sh')
    markdown_targets = []

    foreach manfile : markdown_manfiles
        markdown_target = custom_target(
            'markdown_' + manfile,
            input: manfile,
            output: manfile + '.md',
            command: [
                sh,
                mdoc_to_markdown,
                mandoc,
                '@INPUT@',
                manfile.substring(0, -2),
            ],
            capture: true,
            build_by_default: website_docs,
            install: website_docs,
            install_dir: website_docs_install_dir / 'client' / 'manual' / 'en',
            install_tag: 'doc',
        )
        markdown_targets += markdown_target
    endforeach

    alias_target('docs-markdown', markdown_targets)
endif
