readme_docs_files = [
    'developer.md',
    'features.md',
    'getting_started.md',
    'reporting_bugs.md',
]

website_docs = get_option('website-docs')
website_docs_install_dir = datadir / 'doc' / meson.project_name()

if get_option('website-docs-install-path') != ''
    website_docs_install_dir = get_option('website-docs-install-path')
endif

if website_docs
    install_data(
        readme_docs_files + ['index.md', '_Sidebar.md'],
        install_dir: website_docs_install_dir / 'client' / 'manual' / 'en',
        install_tag: 'doc',
    )
endif

install_data(
    readme_docs_files + ['index.md'],
    install_dir: datadir / 'doc' / 'netatalk-client',
)

subdir('manpages')
