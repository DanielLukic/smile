smile_sources = [
  'emoji_store.c',
  'main.c',
]

smile_sources += emoji_gresource

executable(
  'smile',
  smile_sources,
  dependencies: [
    gtk_dep,
    json_glib_dep,
  ],
  install: true,
)
