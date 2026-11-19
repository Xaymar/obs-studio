# Plugins that need the Knurbs frontend. They replace the frontend-only plugins
# in plugins/ (decklink-output-ui, aja-output-ui, asio-output-ui,
# decklink-captions), which stay untouched so upstream fixes keep merging.

add_subdirectory(device-output)
