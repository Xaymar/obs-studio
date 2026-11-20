# Knurbs frontend

The new Knurbs frontend. It lives beside `frontend/`, which stays untouched so
upstream fixes keep merging, until it reaches parity.

Build with `-DENABLE_VULKAN=ON -DENABLE_KNURBS_FRONTEND=ON`. Needs Qt 6.8 or
newer and Qt Advanced Docking (found as `qt6advanceddocking`, or fetched).

## Layout

- `src/main.cpp`, `src/app/Bootstrap.*`: the only code that runs before
  `QApplication` exists.
- `src/app/`: startup order (`Application`), splash screen, command line,
  paths, log, single instance.
- `src/core/`: `Core` owns libobs and shared state and has no widgets.
  `ModuleLoader` loads plugins in parallel: load, barrier, post-load.
- `src/shell/`: the main window, built on Qt Advanced Docking.
- `src/api/`: `FrontendApi` implements obs-frontend-api for existing plugins.

## Rules for plugins

- Do not create Qt objects in `obs_module_load` or `obs_module_post_load`.
  Wait for `OBS_FRONTEND_EVENT_FINISHED_LOADING`.
- During loading, only register callbacks through obs-frontend-api. Calls that
  need the UI are ignored and logged with the module's name.

## Data folders

Windows: settings in `%APPDATA%\xaymar\knurbs`, logs and crash data in
`%LOCALAPPDATA%\xaymar\knurbs`, caches in `%LOCALAPPDATA%\xaymar\knurbs\cache`.
Linux: `~/.config`, `~/.local/state` and `~/.cache`, each under `xaymar/knurbs`.
`--portable` keeps everything in `data/` next to the program.
