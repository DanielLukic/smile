project(
  'smile',
  'c',
  version: '3.0.0',
  meson_version: '>=0.59.0',
  default_options: [
    'warning_level=2',
    'c_std=c11',
  ],
)

cc = meson.get_compiler('c')
gnome = import('gnome')

common_warnings = cc.get_supported_arguments([
  '-Wall',
  '-Wextra',
  '-Wpedantic',
])

if common_warnings.length() > 0
  add_project_arguments(common_warnings, language: 'c')
endif

gtk_dep = dependency('gtk+-3.0', version: '>=3.24')
json_glib_dep = dependency('json-glib-1.0')

emoji_gresource = gnome.compile_resources(
  'emoji-resources',
  'data/emoji.gresource.xml',
  source_dir: 'data',
  c_name: 'emoji_res',
  export: true,
)

subdir('src')
