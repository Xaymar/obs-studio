IF EXIST deps.zip (curl -kL "%OBS_DEPS_URL%" -f --retry 5 -o deps.zip -z deps.zip) ELSE (curl -kL "%OBS_DEPS_URL%" -f --retry 5 -o deps.zip)
IF EXIST deps.zip (7z x -y deps.zip -ocmbuild)

IF EXIST vlc.zip (curl -kL "https://obsproject.com/downloads/vlc.zip" -f --retry 5 -o vlc.zip -z vlc.zip) ELSE (curl -kL "https://obsproject.com/downloads/vlc.zip" -f --retry 5 -o vlc.zip)
IF EXIST vlc.zip (7z x -y vlc.zip -ovlc)
